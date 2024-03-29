/*
 * This file belongs to the Galois project, a C++ library for exploiting
 * parallelism. The code is being released under the terms of the 3-Clause BSD
 * License (a copy is located in LICENSE.txt at the top-level directory).
 *
 * Copyright (C) 2018, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 */

#ifndef KATANA_LIBGALOIS_KATANA_EXECUTORDETERMINISTIC_H_
#define KATANA_LIBGALOIS_KATANA_EXECUTORDETERMINISTIC_H_

#include <deque>
#include <queue>
#include <type_traits>

#include <boost/iterator/counting_iterator.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/iterator/transform_iterator.hpp>

#include "katana/Bag.h"
#include "katana/Barrier.h"
#include "katana/Executor_ForEach.h"
#include "katana/LoopStatistics.h"
#include "katana/Mem.h"
#include "katana/ParallelSTL.h"
#include "katana/Range.h"
#include "katana/TerminationDetection.h"
#include "katana/ThreadPool.h"
#include "katana/Threads.h"
#include "katana/UnionFind.h"
#include "katana/UserContextAccess.h"
#include "katana/WorkList.h"
#include "katana/config.h"
#include "katana/gIO.h"
#include "katana/gslist.h"

// TODO deterministic hash
// TODO deterministic hash: only give ids to window
// TODO detect and fail if using releasable objects
// TODO fixed neighborhood: cyclic scheduling
// TODO fixed neighborhood: reduce list contention
// TODO fixed neighborhood: profile, reuse graph
// TODO fixed neighborhood: still ~2X slower than implicit version on bfs
namespace katana {

//! Implementation of deterministic execution
namespace internal {

extern thread_local SizedHeapFactory::SizedHeap* dagListHeap;

template <typename T, bool UseLocalState>
class DItemBase {
public:
  T val;
  unsigned long id;

  DItemBase(const T& _val, unsigned long _id) : val(_val), id(_id) {}
  void* getLocalState() const { return nullptr; }
  void setLocalState(void*) {}
};

template <typename T>
class DItemBase<T, true> {
public:
  T val;

private:
  void* localState;

public:
  unsigned long id;

  DItemBase(const T& _val, unsigned long _id)
      : val(_val), localState(nullptr), id(_id) {}
  void* getLocalState() const { return localState; }
  void setLocalState(void* ptr) { localState = ptr; }
};

template <typename OptionsTy>
using DItem =
    DItemBase<typename OptionsTy::value_type, OptionsTy::useLocalState>;

class KATANA_EXPORT FirstPassBase : public SimpleRuntimeContext {
protected:
  bool firstPassFlag;

public:
  explicit FirstPassBase(bool f = true)
      : SimpleRuntimeContext(true), firstPassFlag(f) {}

  ~FirstPassBase() override;

  bool isFirstPass() const { return firstPassFlag; }

  void setFirstPass() { firstPassFlag = true; }

  void resetFirstPass() { firstPassFlag = false; }

  virtual void alwaysAcquire(Lockable*, katana::MethodFlag) = 0;

  void subAcquire(Lockable* lockable, katana::MethodFlag f) override {
    if (isFirstPass()) {
      alwaysAcquire(lockable, f);
    }
  }
};

template <typename OptionsTy, bool HasFixedNeighborhood, bool HasIntentToRead>
class DeterministicContextBase : public FirstPassBase {
public:
  typedef DItem<OptionsTy> Item;
  Item item;

private:
  bool notReady;

public:
  DeterministicContextBase(const Item& _item)
      : FirstPassBase(true), item(_item), notReady(false) {}

  void clear() {}

  bool isReady() { return !notReady; }

  void alwaysAcquire(Lockable* lockable, katana::MethodFlag) override {
    if (this->tryLock(lockable))
      this->addToNhood(lockable);

    DeterministicContextBase* other;
    do {
      other = static_cast<DeterministicContextBase*>(this->getOwner(lockable));
      if (other == this)
        return;
      if (other) {
        bool conflict = other->item.id < this->item.id;
        if (conflict) {
          // A lock that I want but can't get
          notReady = true;
          return;
        }
      }
    } while (!this->stealByCAS(lockable, other));

    // Disable loser
    if (other) {
      // Only need atomic write
      other->notReady = true;
    }
  }

  static void initialize() {}
};

class KATANA_EXPORT HasIntentToReadContext : public FirstPassBase {
public:
  unsigned long id;
  bool notReady;
  bool isWriter;

  ~HasIntentToReadContext() override;

  HasIntentToReadContext(unsigned long id, bool w)
      : FirstPassBase(true), id(id), notReady(false), isWriter(w) {}

  bool isReady() { return !notReady; }
};

class KATANA_EXPORT ReaderContext : public katana::UnionFindNode<ReaderContext>,
                                    public HasIntentToReadContext {
  template <typename, bool, bool>
  friend class DeterministicContextBase;

public:
  ReaderContext(unsigned long id)
      : katana::UnionFindNode<ReaderContext>(const_cast<ReaderContext*>(this)),
        HasIntentToReadContext(id, false) {}

  ~ReaderContext() override;

  void build() {
    if (this->isReady())
      return;
    ReaderContext* r = this->find();
    if (r->isReady())
      r->notReady = true;
  }

  bool propagate() { return this->find()->isReady(); }

  void alwaysAcquire(Lockable*, katana::MethodFlag) override {
    KATANA_LOG_FATAL("unreachable");
  }
};

template <typename OptionsTy>
class DeterministicContextBase<OptionsTy, false, true>
    : public HasIntentToReadContext {
public:
  typedef DItem<OptionsTy> Item;
  Item item;

private:
  ReaderContext readerCtx;

  void acquireRead(Lockable* lockable) {
    HasIntentToReadContext* other;
    do {
      other = static_cast<HasIntentToReadContext*>(this->getOwner(lockable));
      if (other == this || other == &readerCtx)
        return;
      if (other) {
        bool conflict = other->id < this->id;
        if (conflict) {
          if (other->isWriter)
            readerCtx.notReady = true;
          else
            readerCtx.merge(static_cast<ReaderContext*>(other));
          return;
        }
      }
    } while (!readerCtx.stealByCAS(lockable, other));

    // Disable loser
    if (other) {
      if (other->isWriter) {
        // Only need atomic write
        other->notReady = true;
      } else {
        static_cast<ReaderContext*>(other)->merge(&readerCtx);
      }
    }
  }

  void acquireWrite(Lockable* lockable) {
    HasIntentToReadContext* other;
    do {
      other = static_cast<HasIntentToReadContext*>(this->getOwner(lockable));
      if (other == this || other == &readerCtx)
        return;
      if (other) {
        bool conflict = other->id < this->id;
        if (conflict) {
          // A lock that I want but can't get
          this->notReady = true;
          return;
        }
      }
    } while (!this->stealByCAS(lockable, other));

    // Disable loser
    if (other) {
      // Only need atomic write
      other->notReady = true;
    }
  }

public:
  DeterministicContextBase(const Item& i)
      : HasIntentToReadContext(i.id, true), item(i), readerCtx(i.id) {}

  void clear() {}

  void build() { readerCtx.build(); }

  void propagate() {
    if (this->isReady() && !readerCtx.propagate())
      this->notReady = true;
  }

  void alwaysAcquire(Lockable* lockable, katana::MethodFlag m) override {
    KATANA_LOG_DEBUG_ASSERT(m == MethodFlag::READ || m == MethodFlag::WRITE);

    if (this->tryLock(lockable))
      this->addToNhood(lockable);

    if (m == MethodFlag::READ) {
      acquireRead(lockable);
    } else {
      KATANA_LOG_DEBUG_ASSERT(m == MethodFlag::WRITE);
      acquireWrite(lockable);
    }
  }

  static void initialize() {}
};

template <typename OptionsTy>
class DeterministicContextBase<OptionsTy, true, false> : public FirstPassBase {
public:
  typedef DItem<OptionsTy> Item;
  typedef katana::concurrent_gslist<DeterministicContextBase*, 8> ContextList;
  Item item;
  ContextList edges;
  ContextList succs;
  std::atomic<int> preds;

  struct ContextPtrLessThan {
    bool operator()(
        const DeterministicContextBase* a,
        const DeterministicContextBase* b) const {
      // Non-deterministic behavior when we have multiple items with the same
      // id
      if (a->item.id == b->item.id)
        return a < b;
      return a->item.id < b->item.id;
    }
  };

public:
  DeterministicContextBase(const Item& _item)
      : FirstPassBase(true), item(_item), preds(0) {}

  void clear() {
    KATANA_LOG_DEBUG_ASSERT(preds == 0);
    this->commitIteration();
    // TODO replace with bulk heap
    edges.clear(*dagListHeap);
    succs.clear(*dagListHeap);
  }

  void addEdge(DeterministicContextBase* o) {
    succs.push_front(*dagListHeap, o);
    o->preds += 1;
  }

  bool isReady() { return false; }

  virtual void alwaysAcquire(Lockable* lockable, katana::MethodFlag) {
    // First to lock becomes representative
    DeterministicContextBase* owner =
        static_cast<DeterministicContextBase*>(this->getOwner(lockable));
    while (!owner) {
      if (this->tryLock(lockable)) {
        this->setOwner(lockable);
        this->addToNhood(lockable);
      }

      owner = static_cast<DeterministicContextBase*>(this->getOwner(lockable));
    }

    if (std::find(edges.begin(), edges.end(), owner) != edges.end())
      return;
    edges.push_front(*dagListHeap, owner);
  }

  static void initialize() {
    if (!dagListHeap)
      dagListHeap = SizedHeapFactory::getHeapForSize(
          sizeof(typename ContextList::block_type));
  }
};

template <typename OptionsTy>
class DeterministicContextBase<OptionsTy, true, true> {
  // TODO implement me
};

template <typename OptionsTy>
using DeterministicContext = DeterministicContextBase<
    OptionsTy, OptionsTy::hasFixedNeighborhood, OptionsTy::hasIntentToRead>;

template <typename T>
struct DNewItem {
  T val;
  unsigned long parent;
  unsigned count;

  DNewItem(const T& _val, unsigned long _parent, unsigned _count)
      : val(_val), parent(_parent), count(_count) {}

  bool operator<(const DNewItem<T>& o) const {
    if (parent < o.parent)
      return true;
    else if (parent == o.parent)
      return count < o.count;
    else
      return false;
  }

  bool operator==(const DNewItem<T>& o) const {
    return parent == o.parent && count == o.count;
  }

  bool operator!=(const DNewItem<T>& o) const { return !(*this == o); }

  struct GetValue {
    const T& operator()(const DNewItem<T>& x) const { return x.val; }
  };
};

template <typename InputIteratorTy>
void
safe_advance(InputIteratorTy& it, size_t d, size_t& cur, size_t dist) {
  if (d + cur >= dist) {
    d = dist - cur;
  }
  std::advance(it, d);
  cur += d;
}

//! Wrapper around ChunkFIFO to allow peek() and empty() and still
//! have FIFO order
template <int ChunkSize, typename T>
struct FIFO {
  ChunkFIFO<ChunkSize, T, false> m_data;
  ChunkLIFO<16, T, false> m_buffer;
  size_t m_size;

  FIFO() : m_size(0) {}

  ~FIFO() {
    std::optional<T> p;
    while ((p = m_buffer.pop()))
      ;
    while ((p = m_data.pop()))
      ;
  }

  std::optional<T> pop() {
    std::optional<T> p;
    if ((p = m_buffer.pop()) || (p = m_data.pop())) {
      --m_size;
    }
    return p;
  }

  std::optional<T> peek() {
    std::optional<T> p;
    if ((p = m_buffer.pop())) {
      m_buffer.push(*p);
    } else if ((p = m_data.pop())) {
      m_buffer.push(*p);
    }
    return p;
  }

  void push(const T& val) {
    m_data.push(val);
    ++m_size;
  }

  size_t size() const { return m_size; }

  bool empty() const { return m_size == 0; }
};

template <typename T, typename FunctionTy, typename ArgsTy>
struct OptionsCommon {
  typedef T value_type;
  typedef FunctionTy function2_type;
  typedef ArgsTy args_type;

  constexpr static bool needStats = katana::internal::NeedStats<ArgsTy>::value;
  constexpr static bool needsPush = !has_trait<no_pushes_tag, ArgsTy>();
  constexpr static bool needsAborts =
      !has_trait<disable_conflict_detection_tag, ArgsTy>();
  constexpr static bool needsPia = has_trait<per_iter_alloc_tag, ArgsTy>();
  constexpr static bool needsBreak = has_trait<parallel_break_tag, ArgsTy>();

  constexpr static bool hasBreak = has_trait<det_parallel_break_tag, ArgsTy>();
  constexpr static bool hasID = has_trait<det_id_tag, ArgsTy>();

  constexpr static bool useLocalState = has_trait<local_state_tag, ArgsTy>();
  constexpr static bool hasFixedNeighborhood =
      has_trait<fixed_neighborhood_tag, ArgsTy>();
  constexpr static bool hasIntentToRead =
      has_trait<intent_to_read_tag, ArgsTy>();

  static const int ChunkSize = 32;
  static const unsigned InitialNumRounds = 100;
  static const size_t MinDelta = ChunkSize * 40;

  static_assert(
      !hasFixedNeighborhood || (hasFixedNeighborhood && hasID),
      "Please provide id function when operator has fixed neighborhood");

  function2_type fn2;
  args_type args;

  OptionsCommon(function2_type f, ArgsTy a) : fn2(f), args(a) {}
};

template <typename T, typename FunctionTy, typename ArgsTy, bool Enable>
struct OptionsBase : public OptionsCommon<T, FunctionTy, ArgsTy> {
  typedef OptionsCommon<T, FunctionTy, ArgsTy> SuperTy;
  typedef FunctionTy function1_type;

  function1_type fn1;

  OptionsBase(function1_type f, ArgsTy a) : SuperTy(f, a), fn1(f) {}
};

template <typename T, typename FunctionTy, typename ArgsTy>
struct OptionsBase<T, FunctionTy, ArgsTy, true>
    : public OptionsCommon<T, FunctionTy, ArgsTy> {
  typedef OptionsCommon<T, FunctionTy, ArgsTy> SuperTy;
  typedef typename trait_type<neighborhood_visitor_tag, ArgsTy>::type::type
      function1_type;

  function1_type fn1;

  OptionsBase(FunctionTy f, ArgsTy a)
      : SuperTy(f, a),
        fn1(get_trait_value<neighborhood_visitor_tag>(a).value) {}
};

template <typename T, typename FunctionTy, typename ArgsTy>
using Options = OptionsBase<
    T, FunctionTy, ArgsTy, has_trait<neighborhood_visitor_tag, ArgsTy>()>;

template <typename OptionsTy, bool Enable>
class DAGManagerBase {
  typedef DeterministicContext<OptionsTy> Context;

public:
  void destroyDAGManager() {}
  void pushDAGTask(Context*) {}
  bool buildDAG() { return false; }
  template <typename Executor, typename ExecutorTLD>
  bool executeDAG(Executor&, ExecutorTLD&) {
    return false;
  }
};

template <typename OptionsTy>
class DAGManagerBase<OptionsTy, true> {
  typedef DeterministicContext<OptionsTy> Context;
  typedef PerSocketChunkFIFO<OptionsTy::ChunkSize * 2, Context*> WL1;
  typedef PerThreadChunkLIFO<OptionsTy::ChunkSize * 2, Context*> WL2;
  typedef PerSocketChunkFIFO<32, Context*> WL3;

  struct ThreadLocalData : private boost::noncopyable {
    typedef std::vector<
        Context*, typename PerIterAllocTy::rebind<Context*>::other>
        SortBuf;
    IterAllocBaseTy heap;
    PerIterAllocTy alloc;
    SortBuf sortBuf;
    ThreadLocalData() : alloc(&heap), sortBuf(alloc) {}
  };

  PerThreadStorage<ThreadLocalData> data;
  WL1 taskList;
  WL2 taskList2;
  WL3 sourceList;
  TerminationDetection& term;
  Barrier& barrier;

public:
  DAGManagerBase()
      : term(GetTerminationDetection(activeThreads)),
        barrier(GetBarrier(activeThreads)) {}

  void destroyDAGManager() { data.getLocal()->heap.clear(); }

  void pushDAGTask(Context* ctx) { taskList.push(ctx); }

  bool buildDAG() {
    ThreadLocalData& tld = *data.getLocal();
    std::optional<Context*> p;
    while ((p = taskList.pop())) {
      Context* ctx = *p;
      tld.sortBuf.clear();
      std::copy(
          ctx->edges.begin(), ctx->edges.end(),
          std::back_inserter(tld.sortBuf));
      std::sort(
          tld.sortBuf.begin(), tld.sortBuf.end(),
          typename Context::ContextPtrLessThan());

      if (!tld.sortBuf.empty()) {
        Context* last = tld.sortBuf.front();
        for (auto ii = tld.sortBuf.begin() + 1, ei = tld.sortBuf.end();
             ii != ei; ++ii) {
          Context* cur = *ii;
          if (last != cur && cur != ctx)
            last->addEdge(cur);
          last = cur;
        }
      }

      taskList2.push(ctx);
    }
    return true;
  }

  template <typename Executor, typename ExecutorTLD>
  bool executeDAG(Executor& e, ExecutorTLD& etld) {
    auto& local = e.getLocalWindowManager();
    std::optional<Context*> p;
    Context* ctx;

    // Go through all tasks to find intial sources and
    while ((p = taskList2.pop())) {
      ctx = *p;
      if (ctx->preds.load(std::memory_order_relaxed) == 0)
        sourceList.push(ctx);
    }

    term.InitializeThread();

    barrier.Wait();

    size_t oldCommitted = 0;
    size_t committed = 0;
    do {
      std::optional<Context*> p;
      while ((p = sourceList.pop())) {
        ctx = *p;
        KATANA_LOG_DEBUG_ASSERT(ctx->preds == 0);
        bool commit;
        commit = e.executeTask(etld, ctx);
        local.incrementCommitted();
        KATANA_LOG_DEBUG_ASSERT(commit);
        committed += 1;
        e.deallocLocalState(etld.facing);

        if (OptionsTy::needsPia && !OptionsTy::useLocalState)
          etld.facing.resetAlloc();

        etld.facing.resetPushBuffer();

        // enqueue successors
        for (auto& succ : ctx->succs) {
          int v = --succ->preds;
          KATANA_LOG_DEBUG_ASSERT(v >= 0);
          if (v == 0)
            sourceList.push(succ);
        }
      }

      term.SignalWorked(oldCommitted != committed);
      oldCommitted = committed;
      asmPause();
    } while (term.Working());

    if (OptionsTy::needsPia && OptionsTy::useLocalState)
      etld.facing.resetAlloc();

    setThreadContext(0);

    return true;
  }
};

template <typename OptionsTy>
using DAGManager = DAGManagerBase<OptionsTy, OptionsTy::hasFixedNeighborhood>;

template <typename OptionsTy, bool Enable>
struct StateManagerBase {
  typedef typename OptionsTy::value_type value_type;
  typedef typename OptionsTy::function2_type function_type;
  void allocLocalState(UserContextAccess<value_type>&, function_type&) {}
  void deallocLocalState(UserContextAccess<value_type>&) {}
  void saveLocalState(UserContextAccess<value_type>&, DItem<OptionsTy>&) {}
  void restoreLocalState(
      UserContextAccess<value_type>&, const DItem<OptionsTy>&) {}
  void reuseItem(DItem<OptionsTy>&) {}

  template <typename LWL, typename GWL>
  typename GWL::value_type* emplaceContext(
      LWL&, GWL& gwl, const DItem<OptionsTy>& item) const {
    return gwl.emplace(item);
  }

  template <typename LWL, typename GWL>
  typename GWL::value_type* peekContext(LWL&, GWL& gwl) const {
    return gwl.peek();
  }

  template <typename LWL, typename GWL>
  void popContext(LWL&, GWL& gwl) const {
    gwl.pop_peeked();
  }
};

template <typename OptionsTy>
struct StateManagerBase<OptionsTy, true> {
  typedef typename OptionsTy::value_type value_type;
  typedef typename OptionsTy::function2_type function_type;
  typedef typename trait_type<
      local_state_tag, typename OptionsTy::args_type>::type::type LocalState;

  void allocLocalState(UserContextAccess<value_type>& c, function_type&) {
    void* p = c.data().getPerIterAlloc().allocate(sizeof(LocalState));
    // new (p) LocalState(self, c.data().getPerIterAlloc());
    c.setLocalState(p);
  }

  void deallocLocalState(UserContextAccess<value_type>& c) {
    LocalState* p = c.data().template getLocalState<LocalState>();
    if (p)
      p->~LocalState();
  }

  void saveLocalState(
      UserContextAccess<value_type>& c, DItem<OptionsTy>& item) {
    item.setLocalState(c.data().template getLocalState<LocalState>());
  }

  void restoreLocalState(
      UserContextAccess<value_type>& c, const DItem<OptionsTy>& item) {
    c.setLocalState(item.getLocalState());
  }

  template <typename LWL, typename GWL>
  typename LWL::value_type* emplaceContext(
      LWL& lwl, GWL&, const DItem<OptionsTy>& item) const {
    return lwl.emplace(item);
  }

  template <typename LWL, typename GWL>
  typename LWL::value_type* peekContext(LWL& lwl, GWL&) const {
    return lwl.peek();
  }

  template <typename LWL, typename GWL>
  void popContext(LWL& lwl, GWL&) const {
    lwl.pop_peeked();
  }

  void reuseItem(DItem<OptionsTy>& item) { item.setLocalState(nullptr); }
};

template <typename OptionsTy>
using StateManager = StateManagerBase<OptionsTy, OptionsTy::useLocalState>;

template <typename OptionsTy, bool Enable>
class BreakManagerBase {
public:
  bool checkBreak() { return false; }
  BreakManagerBase(const OptionsTy&) {}
};

template <typename OptionsTy>
class BreakManagerBase<OptionsTy, true> {
  typedef typename trait_type<
      det_parallel_break_tag, typename OptionsTy::args_type>::type::type
      BreakFn;
  BreakFn breakFn;
  Barrier& barrier;
  CacheLineStorage<volatile long> done;

public:
  BreakManagerBase(const OptionsTy& o)
      : breakFn(get_trait_value<det_parallel_break_tag>(o.args).value),
        barrier(GetBarrier(activeThreads)) {}

  bool checkBreak() {
    if (ThreadPool::getTID() == 0)
      done.get() = breakFn();
    barrier.Wait();
    return done.get();
  }
};

template <typename OptionsTy>
using BreakManager = BreakManagerBase<OptionsTy, OptionsTy::hasBreak>;

template <typename OptionsTy, bool Enable>
class IntentToReadManagerBase {
  typedef DeterministicContext<OptionsTy> Context;

public:
  void pushIntentToReadTask(Context*) {}
  bool buildIntentToRead() { return false; }
};

template <typename OptionsTy>
class IntentToReadManagerBase<OptionsTy, true> {
  typedef DeterministicContext<OptionsTy> Context;
  typedef katana::gdeque<Context*> WL;
  PerThreadStorage<WL> pending;
  Barrier& barrier;

public:
  IntentToReadManagerBase() : barrier(GetBarrier(activeThreads)) {}

  void pushIntentToReadTask(Context* ctx) {
    pending.getLocal()->push_back(ctx);
  }

  // NB(ddn): Need to gather information from dependees before commitLoop
  // otherwise some contexts will be deallocated before we have time to check
  bool buildIntentToRead() {
    for (Context* ctx : *pending.getLocal())
      ctx->build();
    barrier.Wait();
    for (Context* ctx : *pending.getLocal())
      ctx->propagate();
    pending.getLocal()->clear();
    return true;
  }
};

template <typename OptionsTy>
using IntentToReadManager =
    IntentToReadManagerBase<OptionsTy, OptionsTy::hasIntentToRead>;

template <typename OptionsTy, bool Enable>
class WindowManagerBase {
public:
  class ThreadLocalData {
    template <typename, bool>
    friend class WindowManagerBase;
    size_t window;
    size_t delta;
    size_t committed;
    size_t iterations;

  public:
    size_t nextWindow(bool first = false) {
      if (first)
        window = delta;
      else
        window += delta;
      committed = iterations = 0;
      return window;
    }

    void incrementIterations() { ++iterations; }
    void incrementCommitted() { ++committed; }
  };

private:
  PerThreadStorage<ThreadLocalData> data;
  unsigned numActive;

public:
  WindowManagerBase() { numActive = getActiveThreads(); }

  ThreadLocalData& getLocalWindowManager() { return *data.getLocal(); }

  size_t nextWindow(size_t dist, size_t atleast, size_t base = 0) {
    if (false) {
      // This, which tries to continue delta with new work, seems to result in
      // more conflicts (although less total rounds) and more time
      ThreadLocalData& local = *data.getLocal();
      return local.nextWindow(true);
    } else {
      return initialWindow(dist, atleast, base);
    }
  }

  size_t initialWindow(size_t dist, size_t atleast, size_t base = 0) {
    ThreadLocalData& local = *data.getLocal();
    size_t w = std::max(dist / OptionsTy::InitialNumRounds, atleast) + base;
    local.window = local.delta = w;
    return w;
  }

  void calculateWindow(bool inner) {
    ThreadLocalData& local = *data.getLocal();

    // Accumulate all threads' info
    size_t allcommitted = 0;
    size_t alliterations = 0;
    for (unsigned i = 0; i < numActive; ++i) {
      ThreadLocalData& r = *data.getRemote(i);
      allcommitted += r.committed;
      alliterations += r.iterations;
    }

    float commitRatio =
        alliterations > 0 ? allcommitted / (float)alliterations : 0.0;
    const float target = 0.95;

    if (commitRatio >= target)
      local.delta += local.delta;
    else if (allcommitted == 0) {
      KATANA_LOG_DEBUG_VASSERT(
          (alliterations == 0), "someone should have committed");
      local.delta += local.delta;
    } else
      local.delta = commitRatio / target * local.delta;

    if (!inner) {
      if (local.delta < OptionsTy::MinDelta)
        local.delta = OptionsTy::MinDelta;
    } else if (local.delta < OptionsTy::MinDelta) {
      // Try to get some new work instead of increasing window
      local.delta = 0;
    }

    // Useful debugging info
    if (false) {
      if (ThreadPool::getTID() == 0) {
        char buf[1024];
        snprintf(
            buf, 1024, "%d %.3f (%zu/%zu) window: %zu delta: %zu\n", inner,
            commitRatio, allcommitted, alliterations, local.window,
            local.delta);
        gPrint(buf);
      }
    }
  }
};

template <typename OptionsTy>
class WindowManagerBase<OptionsTy, true> {
public:
  class ThreadLocalData {
  public:
    size_t nextWindow() { return std::numeric_limits<size_t>::max(); }

    void incrementIterations() {}
    void incrementCommitted() {}
  };

private:
  ThreadLocalData data;

public:
  ThreadLocalData& getLocalWindowManager() { return data; }

  size_t nextWindow(size_t, size_t, size_t = 0) { return data.nextWindow(); }

  size_t initialWindow(size_t, size_t, size_t = 0) {
    return std::numeric_limits<size_t>::max();
  }

  void calculateWindow(bool) {}
};

template <typename OptionsTy>
using WindowManager =
    WindowManagerBase<OptionsTy, OptionsTy::hasFixedNeighborhood>;

template <typename OptionsTy, bool Enable>
struct IDManagerBase {
  typedef typename OptionsTy::value_type value_type;
  IDManagerBase(const OptionsTy&) {}
  uintptr_t id(const value_type&) { return 0; }
};

template <typename OptionsTy>
class IDManagerBase<OptionsTy, true> {
  typedef typename OptionsTy::value_type value_type;
  typedef
      typename trait_type<det_id_tag, typename OptionsTy::args_type>::type::type
          IDFn;
  IDFn idFn;

public:
  IDManagerBase(const OptionsTy& o)
      : idFn(get_trait_value<det_id_tag>(o.args).value) {}
  uintptr_t id(const value_type& x) { return idFn(x); }
};

template <typename OptionsTy>
using IDManager = IDManagerBase<OptionsTy, OptionsTy::hasID>;

template <typename OptionsTy>
class NewWorkManager : public IDManager<OptionsTy> {
  typedef typename OptionsTy::value_type value_type;
  typedef DItem<OptionsTy> Item;
  typedef DNewItem<value_type> NewItem;
  typedef std::vector<NewItem, typename PerIterAllocTy::rebind<NewItem>::other>
      NewItemsTy;
  typedef typename NewItemsTy::iterator NewItemsIterator;
  typedef FIFO<1024, Item> ReserveTy;
  typedef PerSocketChunkFIFO<OptionsTy::ChunkSize, NewItem> NewWork;

  struct GetNewItem {
    NewWorkManager* self;
    GetNewItem(NewWorkManager* s = 0) : self(s) {}
    NewItemsTy& operator()(int i) const {
      return self->data.getRemote(i)->newItems;
    }
  };

  typedef boost::transform_iterator<GetNewItem, boost::counting_iterator<int>>
      MergeOuterIt;
  typedef std::vector<NewItem, typename PerIterAllocTy::rebind<NewItem>::other>
      MergeBuf;
  typedef std::vector<
      value_type, typename PerIterAllocTy::rebind<value_type>::other>
      DistributeBuf;

  struct ThreadLocalData {
    IterAllocBaseTy heap;
    PerIterAllocTy alloc;
    NewItemsTy newItems;
    ReserveTy reserve;
    size_t minID;
    size_t maxID;
    size_t size;

    ThreadLocalData() : alloc(&heap), newItems(alloc) {}
  };

  IterAllocBaseTy heap;
  PerIterAllocTy alloc;
  PerThreadStorage<ThreadLocalData> data;
  NewWork new_;
  MergeBuf mergeBuf;
  DistributeBuf distributeBuf;
  Barrier& barrier;
  unsigned numActive;

  bool merge(int begin, int end) {
    if (begin == end)
      return false;
    else if (begin + 1 == end)
      return !data.getRemote(begin)->newItems.empty();

    bool retval = false;
    int mid = (end - begin) / 2 + begin;
    retval |= merge(begin, mid);
    retval |= merge(mid, end);

    GetNewItem fn(this);

    MergeOuterIt bbegin(boost::make_counting_iterator(begin), fn);
    MergeOuterIt mmid(boost::make_counting_iterator(mid), fn);
    MergeOuterIt eend(boost::make_counting_iterator(end), fn);
    auto aa = make_two_level_iterator<
        std::forward_iterator_tag, MergeOuterIt, typename NewItemsTy::iterator,
        GetBegin, GetEnd>(bbegin, mmid);
    auto bb = make_two_level_iterator<
        std::forward_iterator_tag, MergeOuterIt, typename NewItemsTy::iterator,
        GetBegin, GetEnd>(mmid, eend);
    auto cc = make_two_level_iterator<
        std::forward_iterator_tag, MergeOuterIt, typename NewItemsTy::iterator,
        GetBegin, GetEnd>(bbegin, eend);

    while (aa.first != aa.second && bb.first != bb.second) {
      if (*aa.first < *bb.first)
        mergeBuf.push_back(*aa.first++);
      else
        mergeBuf.push_back(*bb.first++);
    }

    for (; aa.first != aa.second; ++aa.first)
      mergeBuf.push_back(*aa.first);

    for (; bb.first != bb.second; ++bb.first)
      mergeBuf.push_back(*bb.first);

    for (NewItemsIterator ii = mergeBuf.begin(), ei = mergeBuf.end(); ii != ei;
         ++ii)
      *cc.first++ = *ii;

    mergeBuf.clear();

    KATANA_LOG_DEBUG_ASSERT(cc.first == cc.second);

    return retval;
  }

  /**
   * Slightly complicated reindexing to separate out continuous elements in
   * InputIterator. <pre> Example:
   *
   * blocksize: 2
   * pos:  0 1 2 3 4 5
   * item: A B C D E F
   * new:  A D B E C F
   * </pre>
   */
  template <typename InputIteratorTy>
  void redistribute(
      InputIteratorTy ii, InputIteratorTy ei, size_t dist, size_t window,
      unsigned tid) {
    // ThreadLocalData& local = *data.getLocal();
    size_t blockSize = window;
    size_t numBlocks = dist / blockSize;

    size_t cur = 0;
    safe_advance(ii, tid, cur, dist);
    while (ii != ei) {
      unsigned long id;
      if (cur < blockSize * numBlocks)
        id = (cur % numBlocks) * blockSize + (cur / numBlocks);
      else
        id = cur;
      distributeBuf[id] = *ii;
      safe_advance(ii, numActive, cur, dist);
    }
  }

  template <typename InputIteratorTy, typename WL>
  void copyMine(
      InputIteratorTy ii, InputIteratorTy ei, size_t dist, WL* wl,
      size_t window, unsigned tid) {
    ThreadLocalData& local = *data.getLocal();
    size_t cur = 0;
    size_t k = 0;
    safe_advance(ii, tid, cur, dist);
    while (ii != ei) {
      unsigned long id = k * numActive + tid;
      if (id < window)
        wl->push(Item(*ii, id));
      else
        break;
      ++k;
      safe_advance(ii, numActive, cur, dist);
    }

    while (ii != ei) {
      unsigned long id = k * numActive + tid;
      local.reserve.push(Item(*ii, id));
      ++k;
      safe_advance(ii, numActive, cur, dist);
    }
  }

  template <typename InputIteratorTy, typename WL>
  void copyAllWithIds(
      InputIteratorTy ii, InputIteratorTy ei, WL* wl, size_t window) {
    ThreadLocalData& local = *data.getLocal();
    for (; ii != ei; ++ii) {
      unsigned long id = ii->parent;
      if (id < window)
        wl->push(Item(ii->val, id));
      else
        break;
    }

    for (; ii != ei; ++ii) {
      unsigned long id = ii->parent;
      local.reserve.push(Item(ii->val, id));
    }
  }

  template <typename InputIteratorTy, typename WL>
  void copyMineAfterRedistribute(
      InputIteratorTy ii, InputIteratorTy ei, size_t dist, WL* wl,
      size_t window, unsigned tid) {
    if (tid == 0) {
      distributeBuf.resize(dist);
    }
    barrier.Wait();
    redistribute(ii, ei, dist, window, tid);
    barrier.Wait();
    copyMine(distributeBuf.begin(), distributeBuf.end(), dist, wl, window, tid);
  }

  template <typename WL>
  void parallelSort(WindowManager<OptionsTy>& wm, WL* wl, unsigned tid) {
    ThreadLocalData& local = *data.getLocal();

    local.newItems.clear();
    std::optional<NewItem> p;
    while ((p = this->new_.pop())) {
      local.newItems.push_back(*p);
    }

    NewItemsIterator ii = local.newItems.begin();
    NewItemsIterator ei = local.newItems.end();
    std::sort(ii, ei);
    initialLimits(ii, ei);
    local.size = local.newItems.size();

    barrier.Wait();

    if (tid == 0) {
      receiveLimits(local);
      broadcastLimits(local);
      if (!OptionsTy::hasID) {
        mergeBuf.reserve(local.size);
        merge(0, numActive);
      }
    }

    barrier.Wait();

    if (OptionsTy::hasID) {
      size_t window = wm.nextWindow(
          local.maxID - local.minID, OptionsTy::MinDelta, local.minID);
      copyAllWithIds(ii, ei, wl, window);
    } else {
      GetNewItem fn(this);
      MergeOuterIt bbegin(boost::make_counting_iterator(0), fn);
      MergeOuterIt eend(boost::make_counting_iterator((int)numActive), fn);
      auto ii = make_two_level_iterator<
          std::forward_iterator_tag, MergeOuterIt,
          typename NewItemsTy::iterator, GetBegin, GetEnd>(bbegin, eend);

      size_t window = wm.nextWindow(local.size, OptionsTy::MinDelta);
      copyMineAfterRedistribute(
          boost::make_transform_iterator(
              ii.first, typename NewItem::GetValue()),
          boost::make_transform_iterator(
              ii.second, typename NewItem::GetValue()),
          local.size, wl, window, tid);
    }
  }

  void broadcastLimits(ThreadLocalData& local) {
    for (unsigned i = 1; i < numActive; ++i) {
      ThreadLocalData& other = *data.getRemote(i);
      other.minID = local.minID;
      other.maxID = local.maxID;
      other.size = local.size;
    }
  }

  void receiveLimits(ThreadLocalData& local) {
    for (unsigned i = 1; i < numActive; ++i) {
      ThreadLocalData& other = *data.getRemote(i);
      local.minID = std::min(other.minID, local.minID);
      local.maxID = std::max(other.maxID, local.maxID);
      local.size += other.size;
    }
  }

  //! Update min and max from sorted iterator
  template <typename BiIteratorTy>
  void initialLimits(BiIteratorTy ii, BiIteratorTy ei) {
    ThreadLocalData& local = *data.getLocal();

    local.minID = std::numeric_limits<size_t>::max();
    local.maxID = std::numeric_limits<size_t>::min();
    local.size = std::distance(ii, ei);

    if (ii != ei) {
      if (ii + 1 == ei) {
        local.minID = local.maxID = ii->parent;
      } else {
        local.minID = ii->parent;
        local.maxID = (ei - 1)->parent;
      }
    }
  }

  template <typename InputIteratorTy>
  void sortInitialWorkDispatch(InputIteratorTy, InputIteratorTy, ...) {}

  template <
      typename InputIteratorTy, bool HasID = OptionsTy::hasID,
      bool HasFixed = OptionsTy::hasFixedNeighborhood>
  auto sortInitialWorkDispatch(InputIteratorTy ii, InputIteratorTy ei, int) ->
      typename std::enable_if<HasID && !HasFixed, void>::type {
    ThreadLocalData& local = *data.getLocal();
    size_t dist = std::distance(ii, ei);

    mergeBuf.reserve(dist);
    for (; ii != ei; ++ii)
      mergeBuf.emplace_back(*ii, this->id(*ii), 1);

    ParallelSTL::sort(mergeBuf.begin(), mergeBuf.end());

    initialLimits(mergeBuf.begin(), mergeBuf.end());
    broadcastLimits(local);
  }

public:
  NewWorkManager(const OptionsTy& o)
      : IDManager<OptionsTy>(o),
        alloc(&heap),
        mergeBuf(alloc),
        distributeBuf(alloc),
        barrier(GetBarrier(activeThreads)) {
    numActive = getActiveThreads();
  }

  bool emptyReserve() { return data.getLocal()->reserve.empty(); }

  template <typename WL>
  void pushNextWindow(WL* wl, size_t window) {
    ThreadLocalData& local = *data.getLocal();
    std::optional<Item> p;
    while ((p = local.reserve.peek())) {
      if (p->id >= window)
        break;
      wl->push(*p);
      local.reserve.pop();
    }
  }

  void clearNewWork() { data.getLocal()->heap.clear(); }

  template <typename InputIteratorTy>
  void sortInitialWork(InputIteratorTy ii, InputIteratorTy ei) {
    return sortInitialWorkDispatch(ii, ei, 0);
  }

  template <typename InputIteratorTy, typename WL>
  void addInitialWork(
      WindowManager<OptionsTy>& wm, InputIteratorTy b, InputIteratorTy e,
      WL* wl) {
    size_t dist = std::distance(b, e);
    if (OptionsTy::hasID) {
      ThreadLocalData& local = *data.getLocal();
      size_t window = wm.initialWindow(dist, OptionsTy::MinDelta, local.minID);
      if (OptionsTy::hasFixedNeighborhood) {
        copyMine(b, e, dist, wl, window, ThreadPool::getTID());
      } else {
        copyMine(
            boost::make_transform_iterator(
                mergeBuf.begin(), typename NewItem::GetValue()),
            boost::make_transform_iterator(
                mergeBuf.end(), typename NewItem::GetValue()),
            mergeBuf.size(), wl, window, ThreadPool::getTID());
      }
    } else {
      size_t window = wm.initialWindow(dist, OptionsTy::MinDelta);
      copyMineAfterRedistribute(b, e, dist, wl, window, ThreadPool::getTID());
    }
  }

  template <bool HasID = OptionsTy::hasID>
  auto pushNew(const value_type& val, unsigned long, unsigned) ->
      typename std::enable_if<HasID, void>::type {
    new_.push(NewItem(val, this->id(val), 1));
  }

  template <bool HasID = OptionsTy::hasID>
  auto pushNew(const value_type& val, unsigned long parent, unsigned count) ->
      typename std::enable_if<!HasID, void>::type {
    new_.push(NewItem(val, parent, count));
  }

  template <typename WL>
  void distributeNewWork(WindowManager<OptionsTy>& wm, WL* wl) {
    parallelSort(wm, wl, ThreadPool::getTID());
  }
};

template <typename OptionsTy>
class Executor : public BreakManager<OptionsTy>,
                 public StateManager<OptionsTy>,
                 public NewWorkManager<OptionsTy>,
                 public WindowManager<OptionsTy>,
                 public DAGManager<OptionsTy>,
                 public IntentToReadManager<OptionsTy> {
  typedef typename OptionsTy::value_type value_type;
  typedef DItem<OptionsTy> Item;
  typedef DeterministicContext<OptionsTy> Context;

  typedef PerSocketChunkFIFO<OptionsTy::ChunkSize, Item> WL;
  typedef PerSocketChunkFIFO<OptionsTy::ChunkSize, Context> PendingWork;
  typedef ChunkFIFO<OptionsTy::ChunkSize, Context, false> LocalPendingWork;

  // Truly thread-local
  using LoopStat = LoopStatistics<OptionsTy::needStats>;
  struct ThreadLocalData : public LoopStat {
    typename OptionsTy::function1_type fn1;
    typename OptionsTy::function2_type fn2;
    LocalPendingWork localPending;
    UserContextAccess<value_type> facing;

    WL* wlcur;
    WL* wlnext;
    size_t rounds;
    size_t outerRounds;
    bool hasNewWork;
    ThreadLocalData(const OptionsTy& o, const char* loopname)
        : LoopStat(loopname),
          fn1(o.fn1),
          fn2(o.fn2),
          rounds(0),
          outerRounds(0) {}
  };

  OptionsTy options;
  Barrier& barrier;
  WL worklists[2];
  PendingWork pending;
  const char* loopname;
  CacheLineStorage<volatile long> innerDone;
  CacheLineStorage<volatile long> outerDone;
  CacheLineStorage<volatile long> hasNewWork;

  int runFunction(ThreadLocalData& tld, Context* ctx);

  bool pendingLoop(ThreadLocalData& tld);
  bool commitLoop(ThreadLocalData& tld);
  void go();

  void drainPending(ThreadLocalData& tld) {
    Context* ctx;
    while ((ctx = this->peekContext(tld.localPending, pending))) {
      ctx->clear();
      this->popContext(tld.localPending, pending);
    }
  }

public:
  Executor(const OptionsTy& o)
      : BreakManager<OptionsTy>(o),
        NewWorkManager<OptionsTy>(o),
        options(o),
        barrier(GetBarrier(activeThreads)),
        loopname(katana::internal::getLoopName(o.args)) {
    static_assert(
        !OptionsTy::needsBreak || OptionsTy::hasBreak,
        "need to use break function to break loop");
  }

  bool executeTask(ThreadLocalData& tld, Context* ctx);

  template <typename RangeTy>
  void initThread(const RangeTy& range) {
    Context::initialize();
    this->addInitialWork(*this, range.begin(), range.end(), &worklists[1]);
  }

  template <typename RangeTy>
  void init(const RangeTy& range) {
    this->sortInitialWork(range.begin(), range.end());
  }

  void operator()() { go(); }
};

template <typename OptionsTy>
void
Executor<OptionsTy>::go() {
  ThreadLocalData tld(options, loopname);
  auto& local = this->getLocalWindowManager();
  tld.wlcur = &worklists[0];
  tld.wlnext = &worklists[1];

  tld.hasNewWork = false;

  while (true) {
    ++tld.outerRounds;

    while (true) {
      ++tld.rounds;

      std::swap(tld.wlcur, tld.wlnext);
      bool nextPending = pendingLoop(tld);
      innerDone.get() = true;

      barrier.Wait();

      if (this->buildDAG())
        barrier.Wait();

      if (this->buildIntentToRead())
        barrier.Wait();

      bool nextCommit = false;
      outerDone.get() = true;

      if (this->executeDAG(*this, tld)) {
        if (OptionsTy::needsBreak)
          barrier.Wait();
        drainPending(tld);
        break;
      }

      nextCommit = commitLoop(tld);

      if (nextPending || nextCommit)
        innerDone.get() = false;

      barrier.Wait();

      if (innerDone.get())
        break;

      this->calculateWindow(true);

      barrier.Wait();

      this->pushNextWindow(tld.wlnext, local.nextWindow());
    }

    if (!this->emptyReserve())
      outerDone.get() = false;

    if (tld.hasNewWork)
      hasNewWork.get() = true;

    if (this->checkBreak())
      break;

    barrier.Wait();

    if (outerDone.get()) {
      if (!OptionsTy::needsPush)
        break;
      if (!hasNewWork.get())  // (1)
        break;
      this->distributeNewWork(*this, tld.wlnext);
      tld.hasNewWork = false;
      // NB: assumes that distributeNewWork has a barrier otherwise checking at
      // (1) is erroneous
      hasNewWork.get() = false;
    } else {
      this->calculateWindow(false);

      this->pushNextWindow(tld.wlnext, local.nextWindow());
    }
  }

  this->destroyDAGManager();
  this->clearNewWork();

  if (OptionsTy::needStats) {
    if (ThreadPool::getTID() == 0) {
      ReportStatSingle(loopname, "RoundsExecuted", tld.rounds);
      ReportStatSingle(loopname, "OuterRoundsExecuted", tld.outerRounds);
    }
  }
}

template <typename OptionsTy>
int
Executor<OptionsTy>::runFunction(ThreadLocalData& tld, Context* ctx) {
  int result = 0;
#ifdef KATANA_USE_LONGJMP_ABORT
  if ((result = setjmp(execFrame)) == 0) {
#elif defined(KATANA_USE_EXCEPTION_ABORT)
  try {
#endif
    tld.fn1(ctx->item.val, tld.facing.data());
#ifdef KATANA_USE_LONGJMP_ABORT
  } else {
    clearConflictLock();
  }
#elif defined(KATANA_USE_EXCEPTION_ABORT)
  } catch (const ConflictFlag& flag) {
    clearConflictLock();
    result = flag;
  }
#endif
  return result;
}

template <typename OptionsTy>
bool
Executor<OptionsTy>::pendingLoop(ThreadLocalData& tld) {
  auto& local = this->getLocalWindowManager();
  bool retval = false;
  std::optional<Item> p;
  while ((p = tld.wlcur->pop())) {
    // Use a new context for each item because there is a race when reusing
    // between aborted iterations.
    Context* ctx = this->emplaceContext(tld.localPending, pending, *p);
    this->pushDAGTask(ctx);
    local.incrementIterations();
    bool commit = true;

    ctx->startIteration();
    ctx->setFirstPass();
    tld.inc_iterations();
    tld.facing.setFirstPass();
    setThreadContext(ctx);

    this->allocLocalState(tld.facing, tld.fn2);
    int result = runFunction(tld, ctx);
    // FIXME:    clearReleasable();
    tld.facing.resetFirstPass();
    ctx->resetFirstPass();
    switch (result) {
    case 0:
    case REACHED_FAILSAFE:
      break;
    case CONFLICT:
      commit = false;
      break;
    default:
      abort();
      break;
    }

    // TODO only needed if fn1 needs pia
    if (OptionsTy::needsPia && !OptionsTy::useLocalState)
      tld.facing.resetAlloc();

    if (commit || OptionsTy::hasFixedNeighborhood) {
      this->saveLocalState(tld.facing, ctx->item);
    } else {
      retval = true;
    }
  }

  return retval;
}

template <typename OptionsTy>
bool
Executor<OptionsTy>::executeTask(ThreadLocalData& tld, Context* ctx) {
  setThreadContext(ctx);
  this->restoreLocalState(tld.facing, ctx->item);
  tld.facing.resetFirstPass();
  ctx->resetFirstPass();
  int result = 0;
#ifdef KATANA_USE_LONGJMP_ABORT
  if ((result = setjmp(execFrame)) == 0) {
#elif defined(KATANA_USE_EXCEPTION_ABORT)
  try {
#endif
    tld.fn2(ctx->item.val, tld.facing.data());
#ifdef KATANA_USE_LONGJMP_ABORT
  } else {
    clearConflictLock();
  }
#elif defined(KATANA_USE_EXCEPTION_ABORT)
  } catch (const ConflictFlag& flag) {
    clearConflictLock();
    result = flag;
  }
#endif
  // FIXME: clearReleasable();
  switch (result) {
  case 0:
    break;
  case CONFLICT:
    return false;
    break;
  default:
    KATANA_LOG_FATAL("unknown conflict flag");
    break;
  }

  if (OptionsTy::needsPush) {
    unsigned long parent = ctx->item.id;
    //    typedef typename UserContextAccess<value_type>::PushBufferTy::iterator
    //    iterator;
    unsigned count = 0;
    for (auto& item : tld.facing.getPushBuffer()) {
      this->pushNew(item, parent, ++count);
      if (count == 0) {
        KATANA_LOG_FATAL("counter overflow");
      }
    }
    if (count)
      tld.hasNewWork = true;
  }
  KATANA_LOG_DEBUG_ASSERT(
      OptionsTy::needsPush ||
      tld.facing.getPushBuffer().begin() == tld.facing.getPushBuffer().end());

  return true;
}

template <typename OptionsTy>
bool
Executor<OptionsTy>::commitLoop(ThreadLocalData& tld) {
  bool retval = false;
  auto& local = this->getLocalWindowManager();

  Context* ctx;
  while ((ctx = this->peekContext(tld.localPending, pending))) {
    bool commit = false;
    if (ctx->isReady())
      commit = executeTask(tld, ctx);

    if (commit) {
      ctx->commitIteration();
      local.incrementCommitted();
    } else {
      this->reuseItem(ctx->item);
      tld.wlnext->push(ctx->item);
      tld.inc_conflicts();
      retval = true;
      ctx->cancelIteration();
    }

    this->deallocLocalState(tld.facing);

    if (OptionsTy::needsPia && !OptionsTy::useLocalState)
      tld.facing.resetAlloc();

    tld.facing.resetPushBuffer();
    ctx->clear();
    this->popContext(tld.localPending, pending);
  }

  if (OptionsTy::needsPia && OptionsTy::useLocalState)
    tld.facing.resetAlloc();

  setThreadContext(0);

  return retval;
}

}  // namespace internal

/**
 * Deterministic execution. Operator should be cautious.
 */
template <typename T = int>
struct Deterministic {
  template <bool _concurrent>
  using rethread = Deterministic<T>;

  template <typename _T>
  using retype = Deterministic<_T>;

  typedef T value_type;
};

template <class T, class FunctionTy, class ArgsTy>
struct ForEachExecutor<Deterministic<T>, FunctionTy, ArgsTy>
    : public internal::Executor<internal::Options<T, FunctionTy, ArgsTy>> {
  typedef internal::Options<T, FunctionTy, ArgsTy> OptionsTy;
  typedef internal::Executor<OptionsTy> SuperTy;
  ForEachExecutor(FunctionTy f, const ArgsTy& args)
      : SuperTy(OptionsTy(f, args)) {}
};

}  // namespace katana
#endif
