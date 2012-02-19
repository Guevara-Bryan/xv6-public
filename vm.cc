#include "types.h"
#include "amd64.h"
#include "mmu.h"
#include "cpu.hh"
#include "kernel.hh"
#include "bits.hh"
#include "spinlock.h"
#include "kalloc.h"
#include "queue.h"
#include "condvar.h"
#include "proc.hh"
#include "vm.hh"
#include "gc.hh"
#include "crange.hh"
#include "cpputil.hh"

enum { vm_debug = 0 };

/*
 * vmnode
 */

vmnode::vmnode(u64 npg, vmntype ntype)
  : npages(npg), ref(0), type(ntype), ip(0), offset(0), sz(0)
{
  if (npg > NELEM(page))
    panic("vmnode too big\n");
  memset(page, 0, sizeof(page));
  if (type == EAGER)
    assert(allocpg() == 0);
}

vmnode::~vmnode()
{
  for(u64 i = 0; i < npages; i++) {
    if (page[i]) {
      kfree(page[i]);
      page[i] = 0;
    }
  }
  if (ip) {
    iput(ip);
    ip = 0;
  }
}

void
vmnode::decref()
{
  if(--ref == 0)
    delete this;
}

int
vmnode::allocpg()
{
  for(u64 i = 0; i < npages; i++) {
    if((page[i] = kalloc()) == 0)
      return -1;
    memset((char *) page[i], 0, PGSIZE);
  }
  return 0;
}

vmnode *
vmnode::copy()
{
  vmnode *c = new vmnode(npages, type);
  if(c != 0) {
    c->type = type;
    if (type == ONDEMAND) {
      c->ip = idup(ip);
      c->offset = offset;
      c->sz = c->sz;
    } 
    if (page[0]) {   // If the first page is present, all of them are present
      if (c->allocpg() < 0) {
	cprintf("vmn_copy: out of memory\n");
	delete c;
	return 0;
      }
      for(u64 i = 0; i < npages; i++) {
	memmove(c->page[i], page[i], PGSIZE);
      }
    }
  }
  return c;
}

int
vmnode::demand_load()
{
  for (u64 i = 0; i < sz; i += PGSIZE) {
    char *p = page[i / PGSIZE];
    s64 n;
    if (sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if (readi(ip, p, offset+i, n) != n)
      return -1;
  }
  return 0;
}

int
vmnode::load(inode *iparg, u64 offarg, u64 szarg)
{
  ip = iparg;
  offset = offarg;
  sz = szarg;

  if (type == ONDEMAND)
    return 0;
  return demand_load();
}

/*
 * vma
 */

vma::vma()
  : rcu_freed("vma"), vma_start(0), vma_end(0), va_type(PRIVATE), n(0)
{
  snprintf(lockname, sizeof(lockname), "vma:%p", this);
  initlock(&lock, lockname, LOCKSTAT_VM);
}

vma::~vma()
{
  if(n)
    n->decref();
  destroylock(&lock);
}

/*
 * vmap
 */

vmap::vmap()
  : cr(10)
{
  snprintf(lockname, sizeof(lockname), "vmap:%p", this);
  initlock(&lock, lockname, LOCKSTAT_VM);

  ref = 1;
  alloc = 0;
  kshared = 0;

  pml4 = setupkvm();
  if (pml4 == 0) {
    cprintf("vmap_alloc: setupkvm out of memory\n");
    goto err0;
  }

  kshared = (char*) ksalloc(slab_kshared);
  if (kshared == NULL) {
    cprintf("vmap::vmap: kshared out of memory\n");
    goto err1;
  }

  if (setupkshared(pml4, kshared)) {
    cprintf("vmap::vmap: setupkshared out of memory\n");
    goto err2;
  }

  return;

 err2:
  ksfree(slab_kshared, kshared);
 err1:
  freevm(pml4);
 err0:
  destroylock(&lock);
}

vmap::~vmap()
{
  if (kshared)
    ksfree(slab_kshared, kshared);
  if (pml4)
    freevm(pml4);
  alloc = 0;
  destroylock(&lock);
}

void
vmap::decref()
{
  if (--ref == 0)
    delete this;
}

vmap*
vmap::copy(int share)
{
  vmap *nm = new vmap();
  if(nm == 0)
    return 0;

  scoped_acquire sa(&lock);
  for (range *r: cr) {
    struct vma *e = (struct vma *) r->value;
    struct vma *ne = new vma();
    if (ne == 0)
      goto err;

    ne->vma_start = e->vma_start;
    ne->vma_end = e->vma_end;
    if (share) {
      ne->n = e->n;
      ne->va_type = COW;

      scoped_acquire sae(&e->lock);
      e->va_type = COW;
      updatepages(pml4, (void *) (e->vma_start), (void *) (e->vma_end), PTE_COW);
    } else {
      ne->n = e->n->copy();
      ne->va_type = e->va_type;
    }

    if (ne->n == 0)
      goto err;

    ne->n->ref++;
    auto span = nm->cr.search_lock(ne->vma_start, ne->vma_end - ne->vma_start);
    for (auto x __attribute__((unused)): span)
      assert(0);  /* span must be empty */
    span.replace(new range(&nm->cr, ne->vma_start, ne->vma_end - ne->vma_start, ne, 0));
  }

  if (share)
    lcr3(v2p(pml4));  // Reload hardware page table

  return nm;

 err:
  delete nm;
  return 0;
}

// Does any vma overlap start..start+len?
// If yes, return the vma pointer.
// If no, return 0.
// This code can't handle regions at the very end
// of the address space, e.g. 0xffffffff..0x0
// We key vma's by their end address.
vma *
vmap::lookup(uptr start, uptr len)
{
  if (start + len < start)
    panic("vmap::lookup bad len");

  range *r = cr.search(start, len);
  if (r != 0) {
    vma *e = (struct vma *) (r->value);
    if (e->vma_end <= e->vma_start)
      panic("malformed va");
    if (e->vma_start < start+len && e->vma_end > start)
      return e;
  }

  return 0;
}

int
vmap::insert(vmnode *n, uptr vma_start)
{
  u64 len = n->npages * PGSIZE;

  auto span = cr.search_lock(vma_start, len);
  for (auto x __attribute__((unused)): span) {
    cprintf("vmap::insert: overlap\n");
    return -1;
  }

  // XXX handle overlaps

  vma *e = new vma();
  if (e == 0)
    return -1;

  e->vma_start = vma_start;
  e->vma_end = vma_start + len;
  e->n = n;
  n->ref++;
  span.replace(new range(&cr, vma_start, len, e, 0));

  updatepages(pml4, (void*) e->vma_start, (void*) (e->vma_end-1), 0);

  return 0;
}

int
vmap::remove(uptr vma_start, uptr len)
{
  uptr vma_end = vma_start + len;

  auto span = cr.search_lock(vma_start, len);
  for (auto x: span) {
    if (x->key < vma_start || x->key + x->size > vma_end) {
      cprintf("vmap::remove: partial unmap not supported\n");
      return -1;
    }
  }

  // XXX handle partial unmap

  span.replace(0);

  updatepages(pml4, (void*) vma_start, (void*) (vma_start + len - 1), 0);
  tlbflush();

  return 0;
}

/*
 * pagefault handling code on vmap
 */

vma *
vmap::pagefault_ondemand(uptr va, u32 err, vma *m, scoped_acquire *mlock)
{
  if (m->n->allocpg() < 0)
    panic("pagefault: couldn't allocate pages");
  mlock->release();
  if (m->n->demand_load() < 0)
    panic("pagefault: couldn't load");
  m = lookup(va, 1);
  if (!m)
    panic("pagefault_ondemand");
  mlock->acquire(&m->lock); // re-acquire lock on m
  return m;
}

int
vmap::pagefault_wcow(uptr va, pme_t *pte, vma *m, u64 npg)
{
  // Always make a copy of n, even if this process has the only ref, 
  // because other processes may change ref count while this process 
  // is handling wcow.
  struct vmnode *n = m->n;
  struct vmnode *c = m->n->copy();
  if (c == 0) {
    cprintf("pagefault_wcow: out of mem\n");
    return -1;
  }
  c->ref = 1;
  m->va_type = PRIVATE;
  m->n = c;
  // Update the hardware page tables to reflect the change to the vma
  updatepages(pml4, (void *) m->vma_start, (void *) m->vma_end, 0);
  pte = walkpgdir(pml4, (const void *)va, 0);
  *pte = v2p(m->n->page[npg]) | PTE_P | PTE_U | PTE_W;
  // drop my ref to vmnode
  n->decref();
  return 0;
}

int
vmap::pagefault(uptr va, u32 err)
{
  pme_t *pte = walkpgdir(pml4, (const void *)va, 1);

  // optimize checks of args to syscals
  if((*pte & (PTE_P|PTE_U|PTE_W)) == (PTE_P|PTE_U|PTE_W))
    return 0;

  scoped_gc_epoch gc;
  vma *m = lookup(va, 1);
  if (m == 0)
    return -1;

  scoped_acquire mlock(&m->lock);
  u64 npg = (PGROUNDDOWN(va) - m->vma_start) / PGSIZE;

  if (m->n && m->n->type == ONDEMAND && m->n->page[npg] == 0)
    m = pagefault_ondemand(va, err, m, &mlock);

  if (vm_debug)
    cprintf("pagefault: err 0x%x va 0x%lx type %d ref %lu pid %d\n",
            err, va, m->va_type, m->n->ref.load(), myproc()->pid);

  if (m->va_type == COW && (err & FEC_WR)) {
    if (pagefault_wcow(va, pte, m, npg) < 0)
      return -1;
  } else if (m->va_type == COW) {
    *pte = v2p(m->n->page[npg]) | PTE_P | PTE_U | PTE_COW;
  } else {
    if (m->n->ref != 1)
      panic("pagefault");
    *pte = v2p(m->n->page[npg]) | PTE_P | PTE_U | PTE_W;
  }

  // XXX(sbw) Why reload hardware page tables?
  lcr3(v2p(pml4));  // Reload hardware page tables
  return 1;
}

int
pagefault(struct vmap *vmap, uptr va, u32 err)
{
  return vmap->pagefault(va, err);
}

// Copy len bytes from p to user address va in vmap.
// Most useful when vmap is not the current page table.
int
vmap::copyout(uptr va, void *p, u64 len)
{
  char *buf = (char*)p;
  while(len > 0){
    uptr va0 = (uptr)PGROUNDDOWN(va);
    scoped_gc_epoch gc;
    vma *vma = lookup(va, 1);
    if(vma == 0)
      return -1;

    acquire(&vma->lock);
    uptr pn = (va0 - vma->vma_start) / PGSIZE;
    char *p0 = vma->n->page[pn];
    if(p0 == 0)
      panic("copyout: missing page");
    uptr n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(p0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
    release(&vma->lock);
  }
  return 0;
}