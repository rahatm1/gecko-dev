/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * This code implements an incremental mark-and-sweep garbage collector, with
 * most sweeping carried out in the background on a parallel thread.
 *
 * Full vs. zone GC
 * ----------------
 *
 * The collector can collect all zones at once, or a subset. These types of
 * collection are referred to as a full GC and a zone GC respectively.
 *
 * The atoms zone is only collected in a full GC since objects in any zone may
 * have pointers to atoms, and these are not recorded in the cross compartment
 * pointer map. Also, the atoms zone is not collected if any thread has an
 * AutoKeepAtoms instance on the stack, or there are any exclusive threads using
 * the runtime.
 *
 * It is possible for an incremental collection that started out as a full GC to
 * become a zone GC if new zones are created during the course of the
 * collection.
 *
 * Incremental collection
 * ----------------------
 *
 * For a collection to be carried out incrementally the following conditions
 * must be met:
 *  - the collection must be run by calling js::GCSlice() rather than js::GC()
 *  - the GC mode must have been set to JSGC_MODE_INCREMENTAL with
 *    JS_SetGCParameter()
 *  - no thread may have an AutoKeepAtoms instance on the stack
 *  - all native objects that have their own trace hook must indicate that they
 *    implement read and write barriers with the JSCLASS_IMPLEMENTS_BARRIERS
 *    flag
 *
 * The last condition is an engine-internal mechanism to ensure that incremental
 * collection is not carried out without the correct barriers being implemented.
 * For more information see 'Incremental marking' below.
 *
 * If the collection is not incremental, all foreground activity happens inside
 * a single call to GC() or GCSlice(). However the collection is not complete
 * until the background sweeping activity has finished.
 *
 * An incremental collection proceeds as a series of slices, interleaved with
 * mutator activity, i.e. running JavaScript code. Slices are limited by a time
 * budget. The slice finishes as soon as possible after the requested time has
 * passed.
 *
 * Collector states
 * ----------------
 *
 * The collector proceeds through the following states, the current state being
 * held in JSRuntime::gcIncrementalState:
 *
 *  - MARK_ROOTS - marks the stack and other roots
 *  - MARK       - incrementally marks reachable things
 *  - SWEEP      - sweeps zones in groups and continues marking unswept zones
 *
 * The MARK_ROOTS activity always takes place in the first slice. The next two
 * states can take place over one or more slices.
 *
 * In other words an incremental collection proceeds like this:
 *
 * Slice 1:   MARK_ROOTS: Roots pushed onto the mark stack.
 *            MARK:       The mark stack is processed by popping an element,
 *                        marking it, and pushing its children.
 *
 *          ... JS code runs ...
 *
 * Slice 2:   MARK:       More mark stack processing.
 *
 *          ... JS code runs ...
 *
 * Slice n-1: MARK:       More mark stack processing.
 *
 *          ... JS code runs ...
 *
 * Slice n:   MARK:       Mark stack is completely drained.
 *            SWEEP:      Select first group of zones to sweep and sweep them.
 *
 *          ... JS code runs ...
 *
 * Slice n+1: SWEEP:      Mark objects in unswept zones that were newly
 *                        identified as alive (see below). Then sweep more zone
 *                        groups.
 *
 *          ... JS code runs ...
 *
 * Slice n+2: SWEEP:      Mark objects in unswept zones that were newly
 *                        identified as alive. Then sweep more zone groups.
 *
 *          ... JS code runs ...
 *
 * Slice m:   SWEEP:      Sweeping is finished, and background sweeping
 *                        started on the helper thread.
 *
 *          ... JS code runs, remaining sweeping done on background thread ...
 *
 * When background sweeping finishes the GC is complete.
 *
 * Incremental marking
 * -------------------
 *
 * Incremental collection requires close collaboration with the mutator (i.e.,
 * JS code) to guarantee correctness.
 *
 *  - During an incremental GC, if a memory location (except a root) is written
 *    to, then the value it previously held must be marked. Write barriers
 *    ensure this.
 *
 *  - Any object that is allocated during incremental GC must start out marked.
 *
 *  - Roots are marked in the first slice and hence don't need write barriers.
 *    Roots are things like the C stack and the VM stack.
 *
 * The problem that write barriers solve is that between slices the mutator can
 * change the object graph. We must ensure that it cannot do this in such a way
 * that makes us fail to mark a reachable object (marking an unreachable object
 * is tolerable).
 *
 * We use a snapshot-at-the-beginning algorithm to do this. This means that we
 * promise to mark at least everything that is reachable at the beginning of
 * collection. To implement it we mark the old contents of every non-root memory
 * location written to by the mutator while the collection is in progress, using
 * write barriers. This is described in gc/Barrier.h.
 *
 * Incremental sweeping
 * --------------------
 *
 * Sweeping is difficult to do incrementally because object finalizers must be
 * run at the start of sweeping, before any mutator code runs. The reason is
 * that some objects use their finalizers to remove themselves from caches. If
 * mutator code was allowed to run after the start of sweeping, it could observe
 * the state of the cache and create a new reference to an object that was just
 * about to be destroyed.
 *
 * Sweeping all finalizable objects in one go would introduce long pauses, so
 * instead sweeping broken up into groups of zones. Zones which are not yet
 * being swept are still marked, so the issue above does not apply.
 *
 * The order of sweeping is restricted by cross compartment pointers - for
 * example say that object |a| from zone A points to object |b| in zone B and
 * neither object was marked when we transitioned to the SWEEP phase. Imagine we
 * sweep B first and then return to the mutator. It's possible that the mutator
 * could cause |a| to become alive through a read barrier (perhaps it was a
 * shape that was accessed via a shape table). Then we would need to mark |b|,
 * which |a| points to, but |b| has already been swept.
 *
 * So if there is such a pointer then marking of zone B must not finish before
 * marking of zone A.  Pointers which form a cycle between zones therefore
 * restrict those zones to being swept at the same time, and these are found
 * using Tarjan's algorithm for finding the strongly connected components of a
 * graph.
 *
 * GC things without finalizers, and things with finalizers that are able to run
 * in the background, are swept on the background thread. This accounts for most
 * of the sweeping work.
 *
 * Reset
 * -----
 *
 * During incremental collection it is possible, although unlikely, for
 * conditions to change such that incremental collection is no longer safe. In
 * this case, the collection is 'reset' by ResetIncrementalGC(). If we are in
 * the mark state, this just stops marking, but if we have started sweeping
 * already, we continue until we have swept the current zone group. Following a
 * reset, a new non-incremental collection is started.
 */

#include "jsgcinlines.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Move.h"

#include <string.h>     /* for memset used when DEBUG */
#ifndef XP_WIN
# include <unistd.h>
#endif

#include "jsapi.h"
#include "jsatom.h"
#include "jscntxt.h"
#include "jscompartment.h"
#include "jsobj.h"
#include "jsscript.h"
#include "jstypes.h"
#include "jsutil.h"
#include "jswatchpoint.h"
#include "jsweakmap.h"
#ifdef XP_WIN
# include "jswin.h"
#endif
#include "prmjtime.h"

#include "gc/FindSCCs.h"
#include "gc/GCInternals.h"
#include "gc/Marking.h"
#include "gc/Memory.h"
#ifdef JS_ION
# include "jit/BaselineJIT.h"
#endif
#include "jit/IonCode.h"
#include "js/SliceBudget.h"
#include "vm/Debugger.h"
#include "vm/ForkJoin.h"
#include "vm/ProxyObject.h"
#include "vm/Shape.h"
#include "vm/String.h"
#include "vm/TraceLogging.h"
#include "vm/WrapperObject.h"

#include "jsobjinlines.h"
#include "jsscriptinlines.h"

#include "vm/Stack-inl.h"
#include "vm/String-inl.h"

using namespace js;
using namespace js::gc;

using mozilla::ArrayEnd;
using mozilla::DebugOnly;
using mozilla::Maybe;
using mozilla::Swap;

/* Perform a Full GC every 20 seconds if MaybeGC is called */
static const uint64_t GC_IDLE_FULL_SPAN = 20 * 1000 * 1000;

/* Increase the IGC marking slice time if we are in highFrequencyGC mode. */
static const int IGC_MARK_SLICE_MULTIPLIER = 2;

#if defined(ANDROID) || defined(MOZ_B2G)
static const int MAX_EMPTY_CHUNK_COUNT = 2;
#else
static const int MAX_EMPTY_CHUNK_COUNT = 30;
#endif

/* This array should be const, but that doesn't link right under GCC. */
const AllocKind gc::slotsToThingKind[] = {
    /* 0 */  FINALIZE_OBJECT0,  FINALIZE_OBJECT2,  FINALIZE_OBJECT2,  FINALIZE_OBJECT4,
    /* 4 */  FINALIZE_OBJECT4,  FINALIZE_OBJECT8,  FINALIZE_OBJECT8,  FINALIZE_OBJECT8,
    /* 8 */  FINALIZE_OBJECT8,  FINALIZE_OBJECT12, FINALIZE_OBJECT12, FINALIZE_OBJECT12,
    /* 12 */ FINALIZE_OBJECT12, FINALIZE_OBJECT16, FINALIZE_OBJECT16, FINALIZE_OBJECT16,
    /* 16 */ FINALIZE_OBJECT16
};

static_assert(JS_ARRAY_LENGTH(slotsToThingKind) == SLOTS_TO_THING_KIND_LIMIT,
              "We have defined a slot count for each kind.");

const uint32_t Arena::ThingSizes[] = {
    sizeof(JSObject),           /* FINALIZE_OBJECT0             */
    sizeof(JSObject),           /* FINALIZE_OBJECT0_BACKGROUND  */
    sizeof(JSObject_Slots2),    /* FINALIZE_OBJECT2             */
    sizeof(JSObject_Slots2),    /* FINALIZE_OBJECT2_BACKGROUND  */
    sizeof(JSObject_Slots4),    /* FINALIZE_OBJECT4             */
    sizeof(JSObject_Slots4),    /* FINALIZE_OBJECT4_BACKGROUND  */
    sizeof(JSObject_Slots8),    /* FINALIZE_OBJECT8             */
    sizeof(JSObject_Slots8),    /* FINALIZE_OBJECT8_BACKGROUND  */
    sizeof(JSObject_Slots12),   /* FINALIZE_OBJECT12            */
    sizeof(JSObject_Slots12),   /* FINALIZE_OBJECT12_BACKGROUND */
    sizeof(JSObject_Slots16),   /* FINALIZE_OBJECT16            */
    sizeof(JSObject_Slots16),   /* FINALIZE_OBJECT16_BACKGROUND */
    sizeof(JSScript),           /* FINALIZE_SCRIPT              */
    sizeof(LazyScript),         /* FINALIZE_LAZY_SCRIPT         */
    sizeof(Shape),              /* FINALIZE_SHAPE               */
    sizeof(BaseShape),          /* FINALIZE_BASE_SHAPE          */
    sizeof(types::TypeObject),  /* FINALIZE_TYPE_OBJECT         */
    sizeof(JSFatInlineString),  /* FINALIZE_FAT_INLINE_STRING   */
    sizeof(JSString),           /* FINALIZE_STRING              */
    sizeof(JSExternalString),   /* FINALIZE_EXTERNAL_STRING     */
    sizeof(jit::JitCode),       /* FINALIZE_JITCODE             */
};

#define OFFSET(type) uint32_t(sizeof(ArenaHeader) + (ArenaSize - sizeof(ArenaHeader)) % sizeof(type))

const uint32_t Arena::FirstThingOffsets[] = {
    OFFSET(JSObject),           /* FINALIZE_OBJECT0             */
    OFFSET(JSObject),           /* FINALIZE_OBJECT0_BACKGROUND  */
    OFFSET(JSObject_Slots2),    /* FINALIZE_OBJECT2             */
    OFFSET(JSObject_Slots2),    /* FINALIZE_OBJECT2_BACKGROUND  */
    OFFSET(JSObject_Slots4),    /* FINALIZE_OBJECT4             */
    OFFSET(JSObject_Slots4),    /* FINALIZE_OBJECT4_BACKGROUND  */
    OFFSET(JSObject_Slots8),    /* FINALIZE_OBJECT8             */
    OFFSET(JSObject_Slots8),    /* FINALIZE_OBJECT8_BACKGROUND  */
    OFFSET(JSObject_Slots12),   /* FINALIZE_OBJECT12            */
    OFFSET(JSObject_Slots12),   /* FINALIZE_OBJECT12_BACKGROUND */
    OFFSET(JSObject_Slots16),   /* FINALIZE_OBJECT16            */
    OFFSET(JSObject_Slots16),   /* FINALIZE_OBJECT16_BACKGROUND */
    OFFSET(JSScript),           /* FINALIZE_SCRIPT              */
    OFFSET(LazyScript),         /* FINALIZE_LAZY_SCRIPT         */
    OFFSET(Shape),              /* FINALIZE_SHAPE               */
    OFFSET(BaseShape),          /* FINALIZE_BASE_SHAPE          */
    OFFSET(types::TypeObject),  /* FINALIZE_TYPE_OBJECT         */
    OFFSET(JSFatInlineString),  /* FINALIZE_FAT_INLINE_STRING   */
    OFFSET(JSString),           /* FINALIZE_STRING              */
    OFFSET(JSExternalString),   /* FINALIZE_EXTERNAL_STRING     */
    OFFSET(jit::JitCode),       /* FINALIZE_JITCODE             */
};

#undef OFFSET

/*
 * Finalization order for incrementally swept things.
 */

static const AllocKind FinalizePhaseStrings[] = {
    FINALIZE_EXTERNAL_STRING
};

static const AllocKind FinalizePhaseScripts[] = {
    FINALIZE_SCRIPT,
    FINALIZE_LAZY_SCRIPT
};

static const AllocKind FinalizePhaseJitCode[] = {
    FINALIZE_JITCODE
};

static const AllocKind * const FinalizePhases[] = {
    FinalizePhaseStrings,
    FinalizePhaseScripts,
    FinalizePhaseJitCode
};
static const int FinalizePhaseCount = sizeof(FinalizePhases) / sizeof(AllocKind*);

static const int FinalizePhaseLength[] = {
    sizeof(FinalizePhaseStrings) / sizeof(AllocKind),
    sizeof(FinalizePhaseScripts) / sizeof(AllocKind),
    sizeof(FinalizePhaseJitCode) / sizeof(AllocKind)
};

static const gcstats::Phase FinalizePhaseStatsPhase[] = {
    gcstats::PHASE_SWEEP_STRING,
    gcstats::PHASE_SWEEP_SCRIPT,
    gcstats::PHASE_SWEEP_JITCODE
};

/*
 * Finalization order for things swept in the background.
 */

static const AllocKind BackgroundPhaseObjects[] = {
    FINALIZE_OBJECT0_BACKGROUND,
    FINALIZE_OBJECT2_BACKGROUND,
    FINALIZE_OBJECT4_BACKGROUND,
    FINALIZE_OBJECT8_BACKGROUND,
    FINALIZE_OBJECT12_BACKGROUND,
    FINALIZE_OBJECT16_BACKGROUND
};

static const AllocKind BackgroundPhaseStrings[] = {
    FINALIZE_FAT_INLINE_STRING,
    FINALIZE_STRING
};

static const AllocKind BackgroundPhaseShapes[] = {
    FINALIZE_SHAPE,
    FINALIZE_BASE_SHAPE,
    FINALIZE_TYPE_OBJECT
};

static const AllocKind * const BackgroundPhases[] = {
    BackgroundPhaseObjects,
    BackgroundPhaseStrings,
    BackgroundPhaseShapes
};
static const int BackgroundPhaseCount = sizeof(BackgroundPhases) / sizeof(AllocKind*);

static const int BackgroundPhaseLength[] = {
    sizeof(BackgroundPhaseObjects) / sizeof(AllocKind),
    sizeof(BackgroundPhaseStrings) / sizeof(AllocKind),
    sizeof(BackgroundPhaseShapes) / sizeof(AllocKind)
};

#ifdef DEBUG
void
ArenaHeader::checkSynchronizedWithFreeList() const
{
    /*
     * Do not allow to access the free list when its real head is still stored
     * in FreeLists and is not synchronized with this one.
     */
    JS_ASSERT(allocated());

    /*
     * We can be called from the background finalization thread when the free
     * list in the zone can mutate at any moment. We cannot do any
     * checks in this case.
     */
    if (IsBackgroundFinalized(getAllocKind()) && zone->runtimeFromAnyThread()->gc.helperThread.onBackgroundThread())
        return;

    FreeSpan firstSpan = FreeSpan::decodeOffsets(arenaAddress(), firstFreeSpanOffsets);
    if (firstSpan.isEmpty())
        return;
    const FreeSpan *list = zone->allocator.arenas.getFreeList(getAllocKind());
    if (list->isEmpty() || firstSpan.arenaAddress() != list->arenaAddress())
        return;

    /*
     * Here this arena has free things, FreeList::lists[thingKind] is not
     * empty and also points to this arena. Thus they must the same.
     */
    JS_ASSERT(firstSpan.isSameNonEmptySpan(list));
}
#endif

/* static */ void
Arena::staticAsserts()
{
    static_assert(JS_ARRAY_LENGTH(ThingSizes) == FINALIZE_LIMIT, "We have defined all thing sizes.");
    static_assert(JS_ARRAY_LENGTH(FirstThingOffsets) == FINALIZE_LIMIT, "We have defined all offsets.");
}

void
Arena::setAsFullyUnused(AllocKind thingKind)
{
    FreeSpan entireList;
    entireList.first = thingsStart(thingKind);
    uintptr_t arenaAddr = aheader.arenaAddress();
    entireList.last = arenaAddr | ArenaMask;
    aheader.setFirstFreeSpan(&entireList);
}

template<typename T>
inline bool
Arena::finalize(FreeOp *fop, AllocKind thingKind, size_t thingSize)
{
    /* Enforce requirements on size of T. */
    JS_ASSERT(thingSize % CellSize == 0);
    JS_ASSERT(thingSize <= 255);

    JS_ASSERT(aheader.allocated());
    JS_ASSERT(thingKind == aheader.getAllocKind());
    JS_ASSERT(thingSize == aheader.getThingSize());
    JS_ASSERT(!aheader.hasDelayedMarking);
    JS_ASSERT(!aheader.markOverflow);
    JS_ASSERT(!aheader.allocatedDuringIncremental);

    uintptr_t firstThing = thingsStart(thingKind);
    uintptr_t firstThingOrSuccessorOfLastMarkedThing = firstThing;
    uintptr_t lastByte = thingsEnd() - 1;

    FreeSpan newListHead;
    FreeSpan *newListTail = &newListHead;
    size_t nmarked = 0;

    for (ArenaCellIterUnderFinalize i(&aheader); !i.done(); i.next()) {
        T *t = i.get<T>();
        if (t->isMarked()) {
            uintptr_t thing = reinterpret_cast<uintptr_t>(t);
            if (thing != firstThingOrSuccessorOfLastMarkedThing) {
                // We just finished passing over one or more free things,
                // so record a new FreeSpan.
                newListTail->first = firstThingOrSuccessorOfLastMarkedThing;
                newListTail->last = thing - thingSize;
                newListTail = newListTail->nextSpanUnchecked(thingSize);
            }
            firstThingOrSuccessorOfLastMarkedThing = thing + thingSize;
            nmarked++;
        } else {
            t->finalize(fop);
            JS_POISON(t, JS_SWEPT_TENURED_PATTERN, thingSize);
        }
    }

    // Complete the last FreeSpan.
    newListTail->first = firstThingOrSuccessorOfLastMarkedThing;
    newListTail->last = lastByte;

    if (nmarked == 0) {
        JS_ASSERT(newListTail == &newListHead);
        JS_ASSERT(newListTail->first == thingsStart(thingKind));
        JS_EXTRA_POISON(data, JS_SWEPT_TENURED_PATTERN, sizeof(data));
        return true;
    }

#ifdef DEBUG
    size_t nfree = 0;
    for (const FreeSpan *span = &newListHead; span != newListTail; span = span->nextSpan()) {
        span->checkSpan();
        JS_ASSERT(Arena::isAligned(span->first, thingSize));
        JS_ASSERT(Arena::isAligned(span->last, thingSize));
        nfree += (span->last - span->first) / thingSize + 1;
    }
    JS_ASSERT(Arena::isAligned(newListTail->first, thingSize));
    JS_ASSERT(newListTail->last == lastByte);
    nfree += (newListTail->last + 1 - newListTail->first) / thingSize;
    JS_ASSERT(nfree + nmarked == thingsPerArena(thingSize));
#endif
    aheader.setFirstFreeSpan(&newListHead);
    return false;
}

/*
 * Insert an arena into the list in appropriate position and update the cursor
 * to ensure that any arena before the cursor is full.
 */
void ArenaList::insert(ArenaHeader *a)
{
    JS_ASSERT(a);
    JS_ASSERT_IF(!head, cursor == &head);
    a->next = *cursor;
    *cursor = a;
    if (!a->hasFreeThings())
        cursor = &a->next;
}

template<typename T>
static inline bool
FinalizeTypedArenas(FreeOp *fop,
                    ArenaHeader **src,
                    ArenaList &dest,
                    AllocKind thingKind,
                    SliceBudget &budget)
{
    /*
     * Finalize arenas from src list, releasing empty arenas and inserting the
     * others into dest in an appropriate position.
     */

    /*
     * During parallel sections, we sometimes finalize the parallel arenas,
     * but in that case, we want to hold on to the memory in our arena
     * lists, not offer it up for reuse.
     */
    bool releaseArenas = !InParallelSection();

    size_t thingSize = Arena::thingSize(thingKind);

    while (ArenaHeader *aheader = *src) {
        *src = aheader->next;
        bool allClear = aheader->getArena()->finalize<T>(fop, thingKind, thingSize);
        if (!allClear)
            dest.insert(aheader);
        else if (releaseArenas)
            aheader->chunk()->releaseArena(aheader);
        else
            aheader->chunk()->recycleArena(aheader, dest, thingKind);

        budget.step(Arena::thingsPerArena(thingSize));
        if (budget.isOverBudget())
            return false;
    }

    return true;
}

/*
 * Finalize the list. On return al->cursor points to the first non-empty arena
 * after the al->head.
 */
static bool
FinalizeArenas(FreeOp *fop,
               ArenaHeader **src,
               ArenaList &dest,
               AllocKind thingKind,
               SliceBudget &budget)
{
    switch(thingKind) {
      case FINALIZE_OBJECT0:
      case FINALIZE_OBJECT0_BACKGROUND:
      case FINALIZE_OBJECT2:
      case FINALIZE_OBJECT2_BACKGROUND:
      case FINALIZE_OBJECT4:
      case FINALIZE_OBJECT4_BACKGROUND:
      case FINALIZE_OBJECT8:
      case FINALIZE_OBJECT8_BACKGROUND:
      case FINALIZE_OBJECT12:
      case FINALIZE_OBJECT12_BACKGROUND:
      case FINALIZE_OBJECT16:
      case FINALIZE_OBJECT16_BACKGROUND:
        return FinalizeTypedArenas<JSObject>(fop, src, dest, thingKind, budget);
      case FINALIZE_SCRIPT:
        return FinalizeTypedArenas<JSScript>(fop, src, dest, thingKind, budget);
      case FINALIZE_LAZY_SCRIPT:
        return FinalizeTypedArenas<LazyScript>(fop, src, dest, thingKind, budget);
      case FINALIZE_SHAPE:
        return FinalizeTypedArenas<Shape>(fop, src, dest, thingKind, budget);
      case FINALIZE_BASE_SHAPE:
        return FinalizeTypedArenas<BaseShape>(fop, src, dest, thingKind, budget);
      case FINALIZE_TYPE_OBJECT:
        return FinalizeTypedArenas<types::TypeObject>(fop, src, dest, thingKind, budget);
      case FINALIZE_STRING:
        return FinalizeTypedArenas<JSString>(fop, src, dest, thingKind, budget);
      case FINALIZE_FAT_INLINE_STRING:
        return FinalizeTypedArenas<JSFatInlineString>(fop, src, dest, thingKind, budget);
      case FINALIZE_EXTERNAL_STRING:
        return FinalizeTypedArenas<JSExternalString>(fop, src, dest, thingKind, budget);
      case FINALIZE_JITCODE:
#ifdef JS_ION
      {
        // JitCode finalization may release references on an executable
        // allocator that is accessed when requesting interrupts.
        JSRuntime::AutoLockForInterrupt lock(fop->runtime());
        return FinalizeTypedArenas<jit::JitCode>(fop, src, dest, thingKind, budget);
      }
#endif
      default:
        MOZ_ASSUME_UNREACHABLE("Invalid alloc kind");
    }
}

static inline Chunk *
AllocChunk(JSRuntime *rt)
{
    return static_cast<Chunk *>(rt->gc.pageAllocator.mapAlignedPages(ChunkSize, ChunkSize));
}

static inline void
FreeChunk(JSRuntime *rt, Chunk *p)
{
    rt->gc.pageAllocator.unmapPages(static_cast<void *>(p), ChunkSize);
}

inline bool
ChunkPool::wantBackgroundAllocation(JSRuntime *rt) const
{
    /*
     * To minimize memory waste we do not want to run the background chunk
     * allocation if we have empty chunks or when the runtime needs just few
     * of them.
     */
    return rt->gc.helperThread.canBackgroundAllocate() &&
           emptyCount == 0 &&
           rt->gc.chunkSet.count() >= 4;
}

/* Must be called with the GC lock taken. */
inline Chunk *
ChunkPool::get(JSRuntime *rt)
{
    JS_ASSERT(this == &rt->gc.chunkPool);

    Chunk *chunk = emptyChunkListHead;
    if (chunk) {
        JS_ASSERT(emptyCount);
        emptyChunkListHead = chunk->info.next;
        --emptyCount;
    } else {
        JS_ASSERT(!emptyCount);
        chunk = Chunk::allocate(rt);
        if (!chunk)
            return nullptr;
        JS_ASSERT(chunk->info.numArenasFreeCommitted == 0);
    }
    JS_ASSERT(chunk->unused());
    JS_ASSERT(!rt->gc.chunkSet.has(chunk));

    if (wantBackgroundAllocation(rt))
        rt->gc.helperThread.startBackgroundAllocationIfIdle();

    return chunk;
}

/* Must be called either during the GC or with the GC lock taken. */
inline void
ChunkPool::put(Chunk *chunk)
{
    chunk->info.age = 0;
    chunk->info.next = emptyChunkListHead;
    emptyChunkListHead = chunk;
    emptyCount++;
}

/* Must be called either during the GC or with the GC lock taken. */
Chunk *
ChunkPool::expire(JSRuntime *rt, bool releaseAll)
{
    JS_ASSERT(this == &rt->gc.chunkPool);

    /*
     * Return old empty chunks to the system while preserving the order of
     * other chunks in the list. This way, if the GC runs several times
     * without emptying the list, the older chunks will stay at the tail
     * and are more likely to reach the max age.
     */
    Chunk *freeList = nullptr;
    int freeChunkCount = 0;
    for (Chunk **chunkp = &emptyChunkListHead; *chunkp; ) {
        JS_ASSERT(emptyCount);
        Chunk *chunk = *chunkp;
        JS_ASSERT(chunk->unused());
        JS_ASSERT(!rt->gc.chunkSet.has(chunk));
        JS_ASSERT(chunk->info.age <= MAX_EMPTY_CHUNK_AGE);
        if (releaseAll || chunk->info.age == MAX_EMPTY_CHUNK_AGE ||
            freeChunkCount++ > MAX_EMPTY_CHUNK_COUNT)
        {
            *chunkp = chunk->info.next;
            --emptyCount;
            chunk->prepareToBeFreed(rt);
            chunk->info.next = freeList;
            freeList = chunk;
        } else {
            /* Keep the chunk but increase its age. */
            ++chunk->info.age;
            chunkp = &chunk->info.next;
        }
    }
    JS_ASSERT_IF(releaseAll, !emptyCount);
    return freeList;
}

static void
FreeChunkList(JSRuntime *rt, Chunk *chunkListHead)
{
    while (Chunk *chunk = chunkListHead) {
        JS_ASSERT(!chunk->info.numArenasFreeCommitted);
        chunkListHead = chunk->info.next;
        FreeChunk(rt, chunk);
    }
}

void
ChunkPool::expireAndFree(JSRuntime *rt, bool releaseAll)
{
    FreeChunkList(rt, expire(rt, releaseAll));
}

/* static */ Chunk *
Chunk::allocate(JSRuntime *rt)
{
    Chunk *chunk = AllocChunk(rt);
    if (!chunk)
        return nullptr;
    chunk->init(rt);
    rt->gc.stats.count(gcstats::STAT_NEW_CHUNK);
    return chunk;
}

/* Must be called with the GC lock taken. */
/* static */ inline void
Chunk::release(JSRuntime *rt, Chunk *chunk)
{
    JS_ASSERT(chunk);
    chunk->prepareToBeFreed(rt);
    FreeChunk(rt, chunk);
}

inline void
Chunk::prepareToBeFreed(JSRuntime *rt)
{
    JS_ASSERT(rt->gc.numArenasFreeCommitted >= info.numArenasFreeCommitted);
    rt->gc.numArenasFreeCommitted -= info.numArenasFreeCommitted;
    rt->gc.stats.count(gcstats::STAT_DESTROY_CHUNK);

#ifdef DEBUG
    /*
     * Let FreeChunkList detect a missing prepareToBeFreed call before it
     * frees chunk.
     */
    info.numArenasFreeCommitted = 0;
#endif
}

void Chunk::decommitAllArenas(JSRuntime *rt)
{
    decommittedArenas.clear(true);
    rt->gc.pageAllocator.markPagesUnused(&arenas[0], ArenasPerChunk * ArenaSize);

    info.freeArenasHead = nullptr;
    info.lastDecommittedArenaOffset = 0;
    info.numArenasFree = ArenasPerChunk;
    info.numArenasFreeCommitted = 0;
}

void
Chunk::init(JSRuntime *rt)
{
    JS_POISON(this, JS_FRESH_TENURED_PATTERN, ChunkSize);

    /*
     * We clear the bitmap to guard against xpc_IsGrayGCThing being called on
     * uninitialized data, which would happen before the first GC cycle.
     */
    bitmap.clear();

    /*
     * Decommit the arenas. We do this after poisoning so that if the OS does
     * not have to recycle the pages, we still get the benefit of poisoning.
     */
    decommitAllArenas(rt);

    /* Initialize the chunk info. */
    info.age = 0;
    info.trailer.location = ChunkLocationTenuredHeap;
    info.trailer.runtime = rt;

    /* The rest of info fields are initialized in PickChunk. */
}

static inline Chunk **
GetAvailableChunkList(Zone *zone)
{
    JSRuntime *rt = zone->runtimeFromAnyThread();
    return zone->isSystem
           ? &rt->gc.systemAvailableChunkListHead
           : &rt->gc.userAvailableChunkListHead;
}

inline void
Chunk::addToAvailableList(Zone *zone)
{
    insertToAvailableList(GetAvailableChunkList(zone));
}

inline void
Chunk::insertToAvailableList(Chunk **insertPoint)
{
    JS_ASSERT(hasAvailableArenas());
    JS_ASSERT(!info.prevp);
    JS_ASSERT(!info.next);
    info.prevp = insertPoint;
    Chunk *insertBefore = *insertPoint;
    if (insertBefore) {
        JS_ASSERT(insertBefore->info.prevp == insertPoint);
        insertBefore->info.prevp = &info.next;
    }
    info.next = insertBefore;
    *insertPoint = this;
}

inline void
Chunk::removeFromAvailableList()
{
    JS_ASSERT(info.prevp);
    *info.prevp = info.next;
    if (info.next) {
        JS_ASSERT(info.next->info.prevp == &info.next);
        info.next->info.prevp = info.prevp;
    }
    info.prevp = nullptr;
    info.next = nullptr;
}

/*
 * Search for and return the next decommitted Arena. Our goal is to keep
 * lastDecommittedArenaOffset "close" to a free arena. We do this by setting
 * it to the most recently freed arena when we free, and forcing it to
 * the last alloc + 1 when we allocate.
 */
uint32_t
Chunk::findDecommittedArenaOffset()
{
    /* Note: lastFreeArenaOffset can be past the end of the list. */
    for (unsigned i = info.lastDecommittedArenaOffset; i < ArenasPerChunk; i++)
        if (decommittedArenas.get(i))
            return i;
    for (unsigned i = 0; i < info.lastDecommittedArenaOffset; i++)
        if (decommittedArenas.get(i))
            return i;
    MOZ_ASSUME_UNREACHABLE("No decommitted arenas found.");
}

ArenaHeader *
Chunk::fetchNextDecommittedArena()
{
    JS_ASSERT(info.numArenasFreeCommitted == 0);
    JS_ASSERT(info.numArenasFree > 0);

    unsigned offset = findDecommittedArenaOffset();
    info.lastDecommittedArenaOffset = offset + 1;
    --info.numArenasFree;
    decommittedArenas.unset(offset);

    Arena *arena = &arenas[offset];
    info.trailer.runtime->gc.pageAllocator.markPagesInUse(arena, ArenaSize);
    arena->aheader.setAsNotAllocated();

    return &arena->aheader;
}

inline ArenaHeader *
Chunk::fetchNextFreeArena(JSRuntime *rt)
{
    JS_ASSERT(info.numArenasFreeCommitted > 0);
    JS_ASSERT(info.numArenasFreeCommitted <= info.numArenasFree);
    JS_ASSERT(info.numArenasFreeCommitted <= rt->gc.numArenasFreeCommitted);

    ArenaHeader *aheader = info.freeArenasHead;
    info.freeArenasHead = aheader->next;
    --info.numArenasFreeCommitted;
    --info.numArenasFree;
    --rt->gc.numArenasFreeCommitted;

    return aheader;
}

ArenaHeader *
Chunk::allocateArena(Zone *zone, AllocKind thingKind)
{
    JS_ASSERT(hasAvailableArenas());

    JSRuntime *rt = zone->runtimeFromAnyThread();
    if (!rt->isHeapMinorCollecting() && rt->gc.bytes >= rt->gc.maxBytes)
        return nullptr;

    ArenaHeader *aheader = MOZ_LIKELY(info.numArenasFreeCommitted > 0)
                           ? fetchNextFreeArena(rt)
                           : fetchNextDecommittedArena();
    aheader->init(zone, thingKind);
    if (MOZ_UNLIKELY(!hasAvailableArenas()))
        removeFromAvailableList();

    rt->gc.bytes += ArenaSize;
    zone->gcBytes += ArenaSize;

    if (zone->gcBytes >= zone->gcTriggerBytes) {
        AutoUnlockGC unlock(rt);
        TriggerZoneGC(zone, JS::gcreason::ALLOC_TRIGGER);
    }

    return aheader;
}

inline void
Chunk::addArenaToFreeList(JSRuntime *rt, ArenaHeader *aheader)
{
    JS_ASSERT(!aheader->allocated());
    aheader->next = info.freeArenasHead;
    info.freeArenasHead = aheader;
    ++info.numArenasFreeCommitted;
    ++info.numArenasFree;
    ++rt->gc.numArenasFreeCommitted;
}

void
Chunk::recycleArena(ArenaHeader *aheader, ArenaList &dest, AllocKind thingKind)
{
    aheader->getArena()->setAsFullyUnused(thingKind);
    dest.insert(aheader);
}

void
Chunk::releaseArena(ArenaHeader *aheader)
{
    JS_ASSERT(aheader->allocated());
    JS_ASSERT(!aheader->hasDelayedMarking);
    Zone *zone = aheader->zone;
    JSRuntime *rt = zone->runtimeFromAnyThread();
    AutoLockGC maybeLock;
    if (rt->gc.helperThread.sweeping())
        maybeLock.lock(rt);

    JS_ASSERT(rt->gc.bytes >= ArenaSize);
    JS_ASSERT(zone->gcBytes >= ArenaSize);
    if (rt->gc.helperThread.sweeping())
        zone->reduceGCTriggerBytes(zone->gcHeapGrowthFactor * ArenaSize);
    rt->gc.bytes -= ArenaSize;
    zone->gcBytes -= ArenaSize;

    aheader->setAsNotAllocated();
    addArenaToFreeList(rt, aheader);

    if (info.numArenasFree == 1) {
        JS_ASSERT(!info.prevp);
        JS_ASSERT(!info.next);
        addToAvailableList(zone);
    } else if (!unused()) {
        JS_ASSERT(info.prevp);
    } else {
        rt->gc.chunkSet.remove(this);
        removeFromAvailableList();
        JS_ASSERT(info.numArenasFree == ArenasPerChunk);
        decommitAllArenas(rt);
        rt->gc.chunkPool.put(this);
    }
}

/* The caller must hold the GC lock. */
static Chunk *
PickChunk(Zone *zone)
{
    JSRuntime *rt = zone->runtimeFromAnyThread();
    Chunk **listHeadp = GetAvailableChunkList(zone);
    Chunk *chunk = *listHeadp;
    if (chunk)
        return chunk;

    chunk = rt->gc.chunkPool.get(rt);
    if (!chunk)
        return nullptr;

    rt->gc.chunkAllocationSinceLastGC = true;

    /*
     * FIXME bug 583732 - chunk is newly allocated and cannot be present in
     * the table so using ordinary lookupForAdd is suboptimal here.
     */
    GCChunkSet::AddPtr p = rt->gc.chunkSet.lookupForAdd(chunk);
    JS_ASSERT(!p);
    if (!rt->gc.chunkSet.add(p, chunk)) {
        Chunk::release(rt, chunk);
        return nullptr;
    }

    chunk->info.prevp = nullptr;
    chunk->info.next = nullptr;
    chunk->addToAvailableList(zone);

    return chunk;
}

js::gc::GCRuntime::GCRuntime(JSRuntime *rt) :
    systemZone(nullptr),
    systemAvailableChunkListHead(nullptr),
    userAvailableChunkListHead(nullptr),
    bytes(0),
    maxBytes(0),
    maxMallocBytes(0),
    numArenasFreeCommitted(0),
    marker(rt),
    verifyPreData(nullptr),
    verifyPostData(nullptr),
    chunkAllocationSinceLastGC(false),
    nextFullGCTime(0),
    lastGCTime(0),
    jitReleaseTime(0),
    allocationThreshold(30 * 1024 * 1024),
    highFrequencyGC(false),
    highFrequencyTimeThreshold(1000),
    highFrequencyLowLimitBytes(100 * 1024 * 1024),
    highFrequencyHighLimitBytes(500 * 1024 * 1024),
    highFrequencyHeapGrowthMax(3.0),
    highFrequencyHeapGrowthMin(1.5),
    lowFrequencyHeapGrowth(1.5),
    dynamicHeapGrowth(false),
    dynamicMarkSlice(false),
    decommitThreshold(32 * 1024 * 1024),
    shouldCleanUpEverything(false),
    grayBitsValid(false),
    isNeeded(0),
    stats(rt),
    number(0),
    startNumber(0),
    isFull(false),
    triggerReason(JS::gcreason::NO_REASON),
    strictCompartmentChecking(false),
#ifdef DEBUG
    disableStrictProxyCheckingCount(0),
#endif
    incrementalState(gc::NO_INCREMENTAL),
    lastMarkSlice(false),
    sweepOnBackgroundThread(false),
    foundBlackGrayEdges(false),
    sweepingZones(nullptr),
    zoneGroupIndex(0),
    zoneGroups(nullptr),
    currentZoneGroup(nullptr),
    sweepPhase(0),
    sweepZone(nullptr),
    sweepKindIndex(0),
    abortSweepAfterCurrentGroup(false),
    arenasAllocatedDuringSweep(nullptr),
#ifdef DEBUG
    markingValidator(nullptr),
#endif
    interFrameGC(0),
    sliceBudget(SliceBudget::Unlimited),
    incrementalEnabled(true),
    generationalDisabled(0),
    manipulatingDeadZones(false),
    objectsMarkedInDeadZones(0),
    poke(false),
    heapState(Idle),
#ifdef JSGC_GENERATIONAL
    nursery(rt),
    storeBuffer(rt, nursery),
#endif
#ifdef JS_GC_ZEAL
    zealMode(0),
    zealFrequency(0),
    nextScheduled(0),
    deterministicOnly(false),
    incrementalLimit(0),
#endif
    validate(true),
    fullCompartmentChecks(false),
    callback(nullptr),
    sliceCallback(nullptr),
    finalizeCallback(nullptr),
    mallocBytes(0),
    mallocGCTriggered(false),
    scriptAndCountsVector(nullptr),
    alwaysPreserveCode(false),
#ifdef DEBUG
    noGCOrAllocationCheck(0),
#endif
    lock(nullptr),
    lockOwner(nullptr),
    helperThread(rt)
{
}

#ifdef JS_GC_ZEAL

extern void
js::SetGCZeal(JSRuntime *rt, uint8_t zeal, uint32_t frequency)
{
    if (rt->gc.verifyPreData)
        VerifyBarriers(rt, PreBarrierVerifier);
    if (rt->gc.verifyPostData)
        VerifyBarriers(rt, PostBarrierVerifier);

#ifdef JSGC_GENERATIONAL
    if (rt->gc.zealMode == ZealGenerationalGCValue) {
        MinorGC(rt, JS::gcreason::DEBUG_GC);
        rt->gc.nursery.leaveZealMode();
    }

    if (zeal == ZealGenerationalGCValue)
        rt->gc.nursery.enterZealMode();
#endif

    bool schedule = zeal >= js::gc::ZealAllocValue;
    rt->gc.zealMode = zeal;
    rt->gc.zealFrequency = frequency;
    rt->gc.nextScheduled = schedule ? frequency : 0;
}

static bool
InitGCZeal(JSRuntime *rt)
{
    const char *env = getenv("JS_GC_ZEAL");
    if (!env)
        return true;

    int zeal = -1;
    int frequency = JS_DEFAULT_ZEAL_FREQ;
    if (strcmp(env, "help") != 0) {
        zeal = atoi(env);
        const char *p = strchr(env, ',');
        if (p)
            frequency = atoi(p + 1);
    }

    if (zeal < 0 || zeal > ZealLimit || frequency < 0) {
        fprintf(stderr,
                "Format: JS_GC_ZEAL=N[,F]\n"
                "N indicates \"zealousness\":\n"
                "  0: no additional GCs\n"
                "  1: additional GCs at common danger points\n"
                "  2: GC every F allocations (default: 100)\n"
                "  3: GC when the window paints (browser only)\n"
                "  4: Verify pre write barriers between instructions\n"
                "  5: Verify pre write barriers between paints\n"
                "  6: Verify stack rooting\n"
                "  7: Collect the nursery every N nursery allocations\n"
                "  8: Incremental GC in two slices: 1) mark roots 2) finish collection\n"
                "  9: Incremental GC in two slices: 1) mark all 2) new marking and finish\n"
                " 10: Incremental GC in multiple slices\n"
                " 11: Verify post write barriers between instructions\n"
                " 12: Verify post write barriers between paints\n"
                " 13: Purge analysis state every F allocations (default: 100)\n");
        return false;
    }

    SetGCZeal(rt, zeal, frequency);
    return true;
}

#endif

/* Lifetime for type sets attached to scripts containing observed types. */
static const int64_t JIT_SCRIPT_RELEASE_TYPES_INTERVAL = 60 * 1000 * 1000;

bool
js_InitGC(JSRuntime *rt, uint32_t maxbytes)
{
    if (!rt->gc.chunkSet.init(INITIAL_CHUNK_CAPACITY))
        return false;

    if (!rt->gc.rootsHash.init(256))
        return false;

    if (!rt->gc.helperThread.init())
        return false;

    /*
     * Separate gcMaxMallocBytes from gcMaxBytes but initialize to maxbytes
     * for default backward API compatibility.
     */
    rt->gc.maxBytes = maxbytes;
    rt->setGCMaxMallocBytes(maxbytes);

#ifndef JS_MORE_DETERMINISTIC
    rt->gc.jitReleaseTime = PRMJ_Now() + JIT_SCRIPT_RELEASE_TYPES_INTERVAL;
#endif

#ifdef JSGC_GENERATIONAL
    if (!rt->gc.nursery.init())
        return false;

    if (!rt->gc.storeBuffer.enable())
        return false;
#endif

#ifdef JS_GC_ZEAL
    if (!InitGCZeal(rt))
        return false;
#endif

    return true;
}

static void
RecordNativeStackTopForGC(JSRuntime *rt)
{
    ConservativeGCData *cgcd = &rt->gc.conservativeGC;

#ifdef JS_THREADSAFE
    /* Record the stack top here only if we are called from a request. */
    if (!rt->requestDepth)
        return;
#endif
    cgcd->recordStackTop();
}

void
js_FinishGC(JSRuntime *rt)
{
    /*
     * Wait until the background finalization stops and the helper thread
     * shuts down before we forcefully release any remaining GC memory.
     */
    rt->gc.helperThread.finish();

#ifdef JS_GC_ZEAL
    /* Free memory associated with GC verification. */
    FinishVerifier(rt);
#endif

    /* Delete all remaining zones. */
    if (rt->gcInitialized) {
        for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
            for (CompartmentsInZoneIter comp(zone); !comp.done(); comp.next())
                js_delete(comp.get());
            js_delete(zone.get());
        }
    }

    rt->gc.zones.clear();

    rt->gc.systemAvailableChunkListHead = nullptr;
    rt->gc.userAvailableChunkListHead = nullptr;
    if (rt->gc.chunkSet.initialized()) {
        for (GCChunkSet::Range r(rt->gc.chunkSet.all()); !r.empty(); r.popFront())
            Chunk::release(rt, r.front());
        rt->gc.chunkSet.clear();
    }

    rt->gc.chunkPool.expireAndFree(rt, true);

    if (rt->gc.rootsHash.initialized())
        rt->gc.rootsHash.clear();

    rt->functionPersistentRooteds.clear();
    rt->idPersistentRooteds.clear();
    rt->objectPersistentRooteds.clear();
    rt->scriptPersistentRooteds.clear();
    rt->stringPersistentRooteds.clear();
    rt->valuePersistentRooteds.clear();
}

template <typename T> struct BarrierOwner {};
template <typename T> struct BarrierOwner<T *> { typedef T result; };
template <> struct BarrierOwner<Value> { typedef HeapValue result; };

template <typename T>
static bool
AddRoot(JSRuntime *rt, T *rp, const char *name, JSGCRootType rootType)
{
    /*
     * Sometimes Firefox will hold weak references to objects and then convert
     * them to strong references by calling AddRoot (e.g., via PreserveWrapper,
     * or ModifyBusyCount in workers). We need a read barrier to cover these
     * cases.
     */
    if (rt->gc.incrementalState != NO_INCREMENTAL)
        BarrierOwner<T>::result::writeBarrierPre(*rp);

    return rt->gc.rootsHash.put((void *)rp, RootInfo(name, rootType));
}

template <typename T>
static bool
AddRoot(JSContext *cx, T *rp, const char *name, JSGCRootType rootType)
{
    bool ok = AddRoot(cx->runtime(), rp, name, rootType);
    if (!ok)
        JS_ReportOutOfMemory(cx);
    return ok;
}

bool
js::AddValueRoot(JSContext *cx, Value *vp, const char *name)
{
    return AddRoot(cx, vp, name, JS_GC_ROOT_VALUE_PTR);
}

extern bool
js::AddValueRootRT(JSRuntime *rt, js::Value *vp, const char *name)
{
    return AddRoot(rt, vp, name, JS_GC_ROOT_VALUE_PTR);
}

extern bool
js::AddStringRoot(JSContext *cx, JSString **rp, const char *name)
{
    return AddRoot(cx, rp, name, JS_GC_ROOT_STRING_PTR);
}

extern bool
js::AddObjectRoot(JSContext *cx, JSObject **rp, const char *name)
{
    return AddRoot(cx, rp, name, JS_GC_ROOT_OBJECT_PTR);
}

extern bool
js::AddObjectRoot(JSRuntime *rt, JSObject **rp, const char *name)
{
    return AddRoot(rt, rp, name, JS_GC_ROOT_OBJECT_PTR);
}

extern bool
js::AddScriptRoot(JSContext *cx, JSScript **rp, const char *name)
{
    return AddRoot(cx, rp, name, JS_GC_ROOT_SCRIPT_PTR);
}

extern JS_FRIEND_API(bool)
js::AddRawValueRoot(JSContext *cx, Value *vp, const char *name)
{
    return AddRoot(cx, vp, name, JS_GC_ROOT_VALUE_PTR);
}

extern JS_FRIEND_API(void)
js::RemoveRawValueRoot(JSContext *cx, Value *vp)
{
    RemoveRoot(cx->runtime(), vp);
}

void
js::RemoveRoot(JSRuntime *rt, void *rp)
{
    rt->gc.rootsHash.remove(rp);
    rt->gc.poke = true;
}

typedef RootedValueMap::Range RootRange;
typedef RootedValueMap::Entry RootEntry;
typedef RootedValueMap::Enum RootEnum;

static size_t
ComputeTriggerBytes(Zone *zone, size_t lastBytes, size_t maxBytes, JSGCInvocationKind gckind)
{
    size_t base = gckind == GC_SHRINK ? lastBytes : Max(lastBytes, zone->runtimeFromMainThread()->gc.allocationThreshold);
    double trigger = double(base) * zone->gcHeapGrowthFactor;
    return size_t(Min(double(maxBytes), trigger));
}

void
Zone::setGCLastBytes(size_t lastBytes, JSGCInvocationKind gckind)
{
    /*
     * The heap growth factor depends on the heap size after a GC and the GC frequency.
     * For low frequency GCs (more than 1sec between GCs) we let the heap grow to 150%.
     * For high frequency GCs we let the heap grow depending on the heap size:
     *   lastBytes < highFrequencyLowLimit: 300%
     *   lastBytes > highFrequencyHighLimit: 150%
     *   otherwise: linear interpolation between 150% and 300% based on lastBytes
     */
    JSRuntime *rt = runtimeFromMainThread();

    if (!rt->gc.dynamicHeapGrowth) {
        gcHeapGrowthFactor = 3.0;
    } else if (lastBytes < 1 * 1024 * 1024) {
        gcHeapGrowthFactor = rt->gc.lowFrequencyHeapGrowth;
    } else {
        JS_ASSERT(rt->gc.highFrequencyHighLimitBytes > rt->gc.highFrequencyLowLimitBytes);
        uint64_t now = PRMJ_Now();
        if (rt->gc.lastGCTime && rt->gc.lastGCTime + rt->gc.highFrequencyTimeThreshold * PRMJ_USEC_PER_MSEC > now) {
            if (lastBytes <= rt->gc.highFrequencyLowLimitBytes) {
                gcHeapGrowthFactor = rt->gc.highFrequencyHeapGrowthMax;
            } else if (lastBytes >= rt->gc.highFrequencyHighLimitBytes) {
                gcHeapGrowthFactor = rt->gc.highFrequencyHeapGrowthMin;
            } else {
                double k = (rt->gc.highFrequencyHeapGrowthMin - rt->gc.highFrequencyHeapGrowthMax)
                           / (double)(rt->gc.highFrequencyHighLimitBytes - rt->gc.highFrequencyLowLimitBytes);
                gcHeapGrowthFactor = (k * (lastBytes - rt->gc.highFrequencyLowLimitBytes)
                                     + rt->gc.highFrequencyHeapGrowthMax);
                JS_ASSERT(gcHeapGrowthFactor <= rt->gc.highFrequencyHeapGrowthMax
                          && gcHeapGrowthFactor >= rt->gc.highFrequencyHeapGrowthMin);
            }
            rt->gc.highFrequencyGC = true;
        } else {
            gcHeapGrowthFactor = rt->gc.lowFrequencyHeapGrowth;
            rt->gc.highFrequencyGC = false;
        }
    }
    gcTriggerBytes = ComputeTriggerBytes(this, lastBytes, rt->gc.maxBytes, gckind);
}

void
Zone::reduceGCTriggerBytes(size_t amount)
{
    JS_ASSERT(amount > 0);
    JS_ASSERT(gcTriggerBytes >= amount);
    if (gcTriggerBytes - amount < runtimeFromAnyThread()->gc.allocationThreshold * gcHeapGrowthFactor)
        return;
    gcTriggerBytes -= amount;
}

Allocator::Allocator(Zone *zone)
  : zone_(zone)
{}

inline void
GCMarker::delayMarkingArena(ArenaHeader *aheader)
{
    if (aheader->hasDelayedMarking) {
        /* Arena already scheduled to be marked later */
        return;
    }
    aheader->setNextDelayedMarking(unmarkedArenaStackTop);
    unmarkedArenaStackTop = aheader;
    markLaterArenas++;
}

void
GCMarker::delayMarkingChildren(const void *thing)
{
    const Cell *cell = reinterpret_cast<const Cell *>(thing);
    cell->arenaHeader()->markOverflow = 1;
    delayMarkingArena(cell->arenaHeader());
}

inline void
ArenaLists::prepareForIncrementalGC(JSRuntime *rt)
{
    for (size_t i = 0; i != FINALIZE_LIMIT; ++i) {
        FreeSpan *headSpan = &freeLists[i];
        if (!headSpan->isEmpty()) {
            ArenaHeader *aheader = headSpan->arenaHeader();
            aheader->allocatedDuringIncremental = true;
            rt->gc.marker.delayMarkingArena(aheader);
        }
    }
}

static inline void
PushArenaAllocatedDuringSweep(JSRuntime *runtime, ArenaHeader *arena)
{
    arena->setNextAllocDuringSweep(runtime->gc.arenasAllocatedDuringSweep);
    runtime->gc.arenasAllocatedDuringSweep = arena;
}

inline void *
ArenaLists::allocateFromArenaInline(Zone *zone, AllocKind thingKind)
{
    /*
     * Parallel JS Note:
     *
     * This function can be called from parallel threads all of which
     * are associated with the same compartment. In that case, each
     * thread will have a distinct ArenaLists.  Therefore, whenever we
     * fall through to PickChunk() we must be sure that we are holding
     * a lock.
     */

    Chunk *chunk = nullptr;

    ArenaList *al = &arenaLists[thingKind];
    AutoLockGC maybeLock;

#ifdef JS_THREADSAFE
    volatile uintptr_t *bfs = &backgroundFinalizeState[thingKind];
    if (*bfs != BFS_DONE) {
        /*
         * We cannot search the arena list for free things while the
         * background finalization runs and can modify head or cursor at any
         * moment. So we always allocate a new arena in that case.
         */
        maybeLock.lock(zone->runtimeFromAnyThread());
        if (*bfs == BFS_RUN) {
            JS_ASSERT(!*al->cursor);
            chunk = PickChunk(zone);
            if (!chunk) {
                /*
                 * Let the caller to wait for the background allocation to
                 * finish and restart the allocation attempt.
                 */
                return nullptr;
            }
        } else if (*bfs == BFS_JUST_FINISHED) {
            /* See comments before BackgroundFinalizeState definition. */
            *bfs = BFS_DONE;
        } else {
            JS_ASSERT(*bfs == BFS_DONE);
        }
    }
#endif /* JS_THREADSAFE */

    if (!chunk) {
        if (ArenaHeader *aheader = *al->cursor) {
            JS_ASSERT(aheader->hasFreeThings());

            /*
             * Normally, the empty arenas are returned to the chunk
             * and should not present on the list. In parallel
             * execution, however, we keep empty arenas in the arena
             * list to avoid synchronizing on the chunk.
             */
            JS_ASSERT(!aheader->isEmpty() || InParallelSection());
            al->cursor = &aheader->next;

            /*
             * Move the free span stored in the arena to the free list and
             * allocate from it.
             */
            freeLists[thingKind] = aheader->getFirstFreeSpan();
            aheader->setAsFullyUsed();
            if (MOZ_UNLIKELY(zone->wasGCStarted())) {
                if (zone->needsBarrier()) {
                    aheader->allocatedDuringIncremental = true;
                    zone->runtimeFromMainThread()->gc.marker.delayMarkingArena(aheader);
                } else if (zone->isGCSweeping()) {
                    PushArenaAllocatedDuringSweep(zone->runtimeFromMainThread(), aheader);
                }
            }
            return freeLists[thingKind].infallibleAllocate(Arena::thingSize(thingKind));
        }

        /* Make sure we hold the GC lock before we call PickChunk. */
        if (!maybeLock.locked())
            maybeLock.lock(zone->runtimeFromAnyThread());
        chunk = PickChunk(zone);
        if (!chunk)
            return nullptr;
    }

    /*
     * While we still hold the GC lock get an arena from some chunk, mark it
     * as full as its single free span is moved to the free lits, and insert
     * it to the list as a fully allocated arena.
     *
     * We add the arena before the the head, not after the tail pointed by the
     * cursor, so after the GC the most recently added arena will be used first
     * for allocations improving cache locality.
     */
    JS_ASSERT(!*al->cursor);
    ArenaHeader *aheader = chunk->allocateArena(zone, thingKind);
    if (!aheader)
        return nullptr;

    if (MOZ_UNLIKELY(zone->wasGCStarted())) {
        if (zone->needsBarrier()) {
            aheader->allocatedDuringIncremental = true;
            zone->runtimeFromMainThread()->gc.marker.delayMarkingArena(aheader);
        } else if (zone->isGCSweeping()) {
            PushArenaAllocatedDuringSweep(zone->runtimeFromMainThread(), aheader);
        }
    }
    aheader->next = al->head;
    if (!al->head) {
        JS_ASSERT(al->cursor == &al->head);
        al->cursor = &aheader->next;
    }
    al->head = aheader;

    /* See comments before allocateFromNewArena about this assert. */
    JS_ASSERT(!aheader->hasFreeThings());
    uintptr_t arenaAddr = aheader->arenaAddress();
    return freeLists[thingKind].allocateFromNewArena(arenaAddr,
                                                     Arena::firstThingOffset(thingKind),
                                                     Arena::thingSize(thingKind));
}

void *
ArenaLists::allocateFromArena(JS::Zone *zone, AllocKind thingKind)
{
    return allocateFromArenaInline(zone, thingKind);
}

void
ArenaLists::wipeDuringParallelExecution(JSRuntime *rt)
{
    JS_ASSERT(InParallelSection());

    // First, check that we all objects we have allocated are eligible
    // for background finalization. The idea is that we will free
    // (below) ALL background finalizable objects, because we know (by
    // the rules of parallel execution) they are not reachable except
    // by other thread-local objects. However, if there were any
    // object ineligible for background finalization, it might retain
    // a reference to one of these background finalizable objects, and
    // that'd be bad.
    for (unsigned i = 0; i < FINALIZE_LAST; i++) {
        AllocKind thingKind = AllocKind(i);
        if (!IsBackgroundFinalized(thingKind) && arenaLists[thingKind].head)
            return;
    }

    // Finalize all background finalizable objects immediately and
    // return the (now empty) arenas back to arena list.
    FreeOp fop(rt, false);
    for (unsigned i = 0; i < FINALIZE_OBJECT_LAST; i++) {
        AllocKind thingKind = AllocKind(i);

        if (!IsBackgroundFinalized(thingKind))
            continue;

        if (arenaLists[i].head) {
            purge(thingKind);
            forceFinalizeNow(&fop, thingKind);
        }
    }
}

void
ArenaLists::finalizeNow(FreeOp *fop, AllocKind thingKind)
{
    JS_ASSERT(!IsBackgroundFinalized(thingKind));
    forceFinalizeNow(fop, thingKind);
}

void
ArenaLists::forceFinalizeNow(FreeOp *fop, AllocKind thingKind)
{
    JS_ASSERT(backgroundFinalizeState[thingKind] == BFS_DONE);

    ArenaHeader *arenas = arenaLists[thingKind].head;
    arenaLists[thingKind].clear();

    SliceBudget budget;
    FinalizeArenas(fop, &arenas, arenaLists[thingKind], thingKind, budget);
    JS_ASSERT(!arenas);
}

void
ArenaLists::queueForForegroundSweep(FreeOp *fop, AllocKind thingKind)
{
    JS_ASSERT(!IsBackgroundFinalized(thingKind));
    JS_ASSERT(backgroundFinalizeState[thingKind] == BFS_DONE);
    JS_ASSERT(!arenaListsToSweep[thingKind]);

    arenaListsToSweep[thingKind] = arenaLists[thingKind].head;
    arenaLists[thingKind].clear();
}

inline void
ArenaLists::queueForBackgroundSweep(FreeOp *fop, AllocKind thingKind)
{
    JS_ASSERT(IsBackgroundFinalized(thingKind));

#ifdef JS_THREADSAFE
    JS_ASSERT(!fop->runtime()->gc.helperThread.sweeping());
#endif

    ArenaList *al = &arenaLists[thingKind];
    if (!al->head) {
        JS_ASSERT(backgroundFinalizeState[thingKind] == BFS_DONE);
        JS_ASSERT(al->cursor == &al->head);
        return;
    }

    /*
     * The state can be done, or just-finished if we have not allocated any GC
     * things from the arena list after the previous background finalization.
     */
    JS_ASSERT(backgroundFinalizeState[thingKind] == BFS_DONE ||
              backgroundFinalizeState[thingKind] == BFS_JUST_FINISHED);

    arenaListsToSweep[thingKind] = al->head;
    al->clear();
    backgroundFinalizeState[thingKind] = BFS_RUN;
}

/*static*/ void
ArenaLists::backgroundFinalize(FreeOp *fop, ArenaHeader *listHead, bool onBackgroundThread)
{
    JS_ASSERT(listHead);
    AllocKind thingKind = listHead->getAllocKind();
    Zone *zone = listHead->zone;

    ArenaList finalized;
    SliceBudget budget;
    FinalizeArenas(fop, &listHead, finalized, thingKind, budget);
    JS_ASSERT(!listHead);

    /*
     * After we finish the finalization al->cursor must point to the end of
     * the head list as we emptied the list before the background finalization
     * and the allocation adds new arenas before the cursor.
     */
    ArenaLists *lists = &zone->allocator.arenas;
    ArenaList *al = &lists->arenaLists[thingKind];

    AutoLockGC lock(fop->runtime());
    JS_ASSERT(lists->backgroundFinalizeState[thingKind] == BFS_RUN);
    JS_ASSERT(!*al->cursor);

    if (finalized.head) {
        *al->cursor = finalized.head;
        if (finalized.cursor != &finalized.head)
            al->cursor = finalized.cursor;
    }

    /*
     * We must set the state to BFS_JUST_FINISHED if we are running on the
     * background thread and we have touched arenaList list, even if we add to
     * the list only fully allocated arenas without any free things. It ensures
     * that the allocation thread takes the GC lock and all writes to the free
     * list elements are propagated. As we always take the GC lock when
     * allocating new arenas from the chunks we can set the state to BFS_DONE if
     * we have released all finalized arenas back to their chunks.
     */
    if (onBackgroundThread && finalized.head)
        lists->backgroundFinalizeState[thingKind] = BFS_JUST_FINISHED;
    else
        lists->backgroundFinalizeState[thingKind] = BFS_DONE;

    lists->arenaListsToSweep[thingKind] = nullptr;
}

void
ArenaLists::queueObjectsForSweep(FreeOp *fop)
{
    gcstats::AutoPhase ap(fop->runtime()->gc.stats, gcstats::PHASE_SWEEP_OBJECT);

    finalizeNow(fop, FINALIZE_OBJECT0);
    finalizeNow(fop, FINALIZE_OBJECT2);
    finalizeNow(fop, FINALIZE_OBJECT4);
    finalizeNow(fop, FINALIZE_OBJECT8);
    finalizeNow(fop, FINALIZE_OBJECT12);
    finalizeNow(fop, FINALIZE_OBJECT16);

    queueForBackgroundSweep(fop, FINALIZE_OBJECT0_BACKGROUND);
    queueForBackgroundSweep(fop, FINALIZE_OBJECT2_BACKGROUND);
    queueForBackgroundSweep(fop, FINALIZE_OBJECT4_BACKGROUND);
    queueForBackgroundSweep(fop, FINALIZE_OBJECT8_BACKGROUND);
    queueForBackgroundSweep(fop, FINALIZE_OBJECT12_BACKGROUND);
    queueForBackgroundSweep(fop, FINALIZE_OBJECT16_BACKGROUND);
}

void
ArenaLists::queueStringsForSweep(FreeOp *fop)
{
    gcstats::AutoPhase ap(fop->runtime()->gc.stats, gcstats::PHASE_SWEEP_STRING);

    queueForBackgroundSweep(fop, FINALIZE_FAT_INLINE_STRING);
    queueForBackgroundSweep(fop, FINALIZE_STRING);

    queueForForegroundSweep(fop, FINALIZE_EXTERNAL_STRING);
}

void
ArenaLists::queueScriptsForSweep(FreeOp *fop)
{
    gcstats::AutoPhase ap(fop->runtime()->gc.stats, gcstats::PHASE_SWEEP_SCRIPT);
    queueForForegroundSweep(fop, FINALIZE_SCRIPT);
    queueForForegroundSweep(fop, FINALIZE_LAZY_SCRIPT);
}

void
ArenaLists::queueJitCodeForSweep(FreeOp *fop)
{
    gcstats::AutoPhase ap(fop->runtime()->gc.stats, gcstats::PHASE_SWEEP_JITCODE);
    queueForForegroundSweep(fop, FINALIZE_JITCODE);
}

void
ArenaLists::queueShapesForSweep(FreeOp *fop)
{
    gcstats::AutoPhase ap(fop->runtime()->gc.stats, gcstats::PHASE_SWEEP_SHAPE);

    queueForBackgroundSweep(fop, FINALIZE_SHAPE);
    queueForBackgroundSweep(fop, FINALIZE_BASE_SHAPE);
    queueForBackgroundSweep(fop, FINALIZE_TYPE_OBJECT);
}

static void *
RunLastDitchGC(JSContext *cx, JS::Zone *zone, AllocKind thingKind)
{
    /*
     * In parallel sections, we do not attempt to refill the free list
     * and hence do not encounter last ditch GC.
     */
    JS_ASSERT(!InParallelSection());

    PrepareZoneForGC(zone);

    JSRuntime *rt = cx->runtime();

    /* The last ditch GC preserves all atoms. */
    AutoKeepAtoms keepAtoms(cx->perThreadData);
    GC(rt, GC_NORMAL, JS::gcreason::LAST_DITCH);

    /*
     * The JSGC_END callback can legitimately allocate new GC
     * things and populate the free list. If that happens, just
     * return that list head.
     */
    size_t thingSize = Arena::thingSize(thingKind);
    if (void *thing = zone->allocator.arenas.allocateFromFreeList(thingKind, thingSize))
        return thing;

    return nullptr;
}

template <AllowGC allowGC>
/* static */ void *
ArenaLists::refillFreeList(ThreadSafeContext *cx, AllocKind thingKind)
{
    JS_ASSERT(cx->allocator()->arenas.freeLists[thingKind].isEmpty());
    JS_ASSERT_IF(cx->isJSContext(), !cx->asJSContext()->runtime()->isHeapBusy());

    Zone *zone = cx->allocator()->zone_;

    bool runGC = cx->allowGC() && allowGC &&
                 cx->asJSContext()->runtime()->gc.incrementalState != NO_INCREMENTAL &&
                 zone->gcBytes > zone->gcTriggerBytes;

#ifdef JS_THREADSAFE
    JS_ASSERT_IF(cx->isJSContext() && allowGC,
                 !cx->asJSContext()->runtime()->currentThreadHasExclusiveAccess());
#endif

    for (;;) {
        if (MOZ_UNLIKELY(runGC)) {
            if (void *thing = RunLastDitchGC(cx->asJSContext(), zone, thingKind))
                return thing;
        }

        if (cx->isJSContext()) {
            /*
             * allocateFromArena may fail while the background finalization still
             * run. If we are on the main thread, we want to wait for it to finish
             * and restart. However, checking for that is racy as the background
             * finalization could free some things after allocateFromArena decided
             * to fail but at this point it may have already stopped. To avoid
             * this race we always try to allocate twice.
             */
            for (bool secondAttempt = false; ; secondAttempt = true) {
                void *thing = cx->allocator()->arenas.allocateFromArenaInline(zone, thingKind);
                if (MOZ_LIKELY(!!thing))
                    return thing;
                if (secondAttempt)
                    break;

                cx->asJSContext()->runtime()->gc.helperThread.waitBackgroundSweepEnd();
            }
        } else {
#ifdef JS_THREADSAFE
            /*
             * If we're off the main thread, we try to allocate once and
             * return whatever value we get. If we aren't in a ForkJoin
             * session (i.e. we are in a worker thread async with the main
             * thread), we need to first ensure the main thread is not in a GC
             * session.
             */
            mozilla::Maybe<AutoLockWorkerThreadState> lock;
            JSRuntime *rt = zone->runtimeFromAnyThread();
            if (rt->exclusiveThreadsPresent()) {
                lock.construct();
                while (rt->isHeapBusy())
                    WorkerThreadState().wait(GlobalWorkerThreadState::PRODUCER);
            }

            void *thing = cx->allocator()->arenas.allocateFromArenaInline(zone, thingKind);
            if (thing)
                return thing;
#else
            MOZ_CRASH();
#endif
        }

        if (!cx->allowGC() || !allowGC)
            return nullptr;

        /*
         * We failed to allocate. Run the GC if we haven't done it already.
         * Otherwise report OOM.
         */
        if (runGC)
            break;
        runGC = true;
    }

    JS_ASSERT(allowGC);
    js_ReportOutOfMemory(cx);
    return nullptr;
}

template void *
ArenaLists::refillFreeList<NoGC>(ThreadSafeContext *cx, AllocKind thingKind);

template void *
ArenaLists::refillFreeList<CanGC>(ThreadSafeContext *cx, AllocKind thingKind);

JSGCTraceKind
js_GetGCThingTraceKind(void *thing)
{
    return GetGCThingTraceKind(thing);
}

/* static */ int64_t
SliceBudget::TimeBudget(int64_t millis)
{
    return millis * PRMJ_USEC_PER_MSEC;
}

/* static */ int64_t
SliceBudget::WorkBudget(int64_t work)
{
    /* For work = 0 not to mean Unlimited, we subtract 1. */
    return -work - 1;
}

SliceBudget::SliceBudget()
  : deadline(INT64_MAX),
    counter(INTPTR_MAX)
{
}

SliceBudget::SliceBudget(int64_t budget)
{
    if (budget == Unlimited) {
        deadline = INT64_MAX;
        counter = INTPTR_MAX;
    } else if (budget > 0) {
        deadline = PRMJ_Now() + budget;
        counter = CounterReset;
    } else {
        deadline = 0;
        counter = -budget - 1;
    }
}

bool
SliceBudget::checkOverBudget()
{
    bool over = PRMJ_Now() > deadline;
    if (!over)
        counter = CounterReset;
    return over;
}

void
js::MarkCompartmentActive(InterpreterFrame *fp)
{
    fp->script()->compartment()->zone()->active = true;
}

static void
RequestInterrupt(JSRuntime *rt, JS::gcreason::Reason reason)
{
    if (rt->gc.isNeeded)
        return;

    rt->gc.isNeeded = true;
    rt->gc.triggerReason = reason;
    rt->requestInterrupt(JSRuntime::RequestInterruptMainThread);
}

bool
js::TriggerGC(JSRuntime *rt, JS::gcreason::Reason reason)
{
    /* Wait till end of parallel section to trigger GC. */
    if (InParallelSection()) {
        ForkJoinContext::current()->requestGC(reason);
        return true;
    }

    /* Don't trigger GCs when allocating under the interrupt callback lock. */
    if (rt->currentThreadOwnsInterruptLock())
        return false;

    JS_ASSERT(CurrentThreadCanAccessRuntime(rt));

    /* GC is already running. */
    if (rt->isHeapCollecting())
        return false;

    JS::PrepareForFullGC(rt);
    RequestInterrupt(rt, reason);
    return true;
}

bool
js::TriggerZoneGC(Zone *zone, JS::gcreason::Reason reason)
{
    /*
     * If parallel threads are running, wait till they
     * are stopped to trigger GC.
     */
    if (InParallelSection()) {
        ForkJoinContext::current()->requestZoneGC(zone, reason);
        return true;
    }

    /* Zones in use by a thread with an exclusive context can't be collected. */
    if (zone->usedByExclusiveThread)
        return false;

    JSRuntime *rt = zone->runtimeFromMainThread();

    /* Don't trigger GCs when allocating under the interrupt callback lock. */
    if (rt->currentThreadOwnsInterruptLock())
        return false;

    /* GC is already running. */
    if (rt->isHeapCollecting())
        return false;

    if (rt->gcZeal() == ZealAllocValue) {
        TriggerGC(rt, reason);
        return true;
    }

    if (rt->isAtomsZone(zone)) {
        /* We can't do a zone GC of the atoms compartment. */
        TriggerGC(rt, reason);
        return true;
    }

    PrepareZoneForGC(zone);
    RequestInterrupt(rt, reason);
    return true;
}

void
js::MaybeGC(JSContext *cx)
{
    JSRuntime *rt = cx->runtime();
    JS_ASSERT(CurrentThreadCanAccessRuntime(rt));

    if (rt->gcZeal() == ZealAllocValue || rt->gcZeal() == ZealPokeValue) {
        JS::PrepareForFullGC(rt);
        GC(rt, GC_NORMAL, JS::gcreason::MAYBEGC);
        return;
    }

    if (rt->gc.isNeeded) {
        GCSlice(rt, GC_NORMAL, JS::gcreason::MAYBEGC);
        return;
    }

    double factor = rt->gc.highFrequencyGC ? 0.85 : 0.9;
    Zone *zone = cx->zone();
    if (zone->gcBytes > 1024 * 1024 &&
        zone->gcBytes >= factor * zone->gcTriggerBytes &&
        rt->gc.incrementalState == NO_INCREMENTAL &&
        !rt->gc.helperThread.sweeping())
    {
        PrepareZoneForGC(zone);
        GCSlice(rt, GC_NORMAL, JS::gcreason::MAYBEGC);
        return;
    }

#ifndef JS_MORE_DETERMINISTIC
    /*
     * Access to the counters and, on 32 bit, setting gcNextFullGCTime below
     * is not atomic and a race condition could trigger or suppress the GC. We
     * tolerate this.
     */
    int64_t now = PRMJ_Now();
    if (rt->gc.nextFullGCTime && rt->gc.nextFullGCTime <= now) {
        if (rt->gc.chunkAllocationSinceLastGC ||
            rt->gc.numArenasFreeCommitted > rt->gc.decommitThreshold)
        {
            JS::PrepareForFullGC(rt);
            GCSlice(rt, GC_SHRINK, JS::gcreason::MAYBEGC);
        } else {
            rt->gc.nextFullGCTime = now + GC_IDLE_FULL_SPAN;
        }
    }
#endif
}

static void
DecommitArenasFromAvailableList(JSRuntime *rt, Chunk **availableListHeadp)
{
    Chunk *chunk = *availableListHeadp;
    if (!chunk)
        return;

    /*
     * Decommit is expensive so we avoid holding the GC lock while calling it.
     *
     * We decommit from the tail of the list to minimize interference with the
     * main thread that may start to allocate things at this point.
     *
     * The arena that is been decommitted outside the GC lock must not be
     * available for allocations either via the free list or via the
     * decommittedArenas bitmap. For that we just fetch the arena from the
     * free list before the decommit pretending as it was allocated. If this
     * arena also is the single free arena in the chunk, then we must remove
     * from the available list before we release the lock so the allocation
     * thread would not see chunks with no free arenas on the available list.
     *
     * After we retake the lock, we mark the arena as free and decommitted if
     * the decommit was successful. We must also add the chunk back to the
     * available list if we removed it previously or when the main thread
     * have allocated all remaining free arenas in the chunk.
     *
     * We also must make sure that the aheader is not accessed again after we
     * decommit the arena.
     */
    JS_ASSERT(chunk->info.prevp == availableListHeadp);
    while (Chunk *next = chunk->info.next) {
        JS_ASSERT(next->info.prevp == &chunk->info.next);
        chunk = next;
    }

    for (;;) {
        while (chunk->info.numArenasFreeCommitted != 0) {
            ArenaHeader *aheader = chunk->fetchNextFreeArena(rt);

            Chunk **savedPrevp = chunk->info.prevp;
            if (!chunk->hasAvailableArenas())
                chunk->removeFromAvailableList();

            size_t arenaIndex = Chunk::arenaIndex(aheader->arenaAddress());
            bool ok;
            {
                /*
                 * If the main thread waits for the decommit to finish, skip
                 * potentially expensive unlock/lock pair on the contested
                 * lock.
                 */
                Maybe<AutoUnlockGC> maybeUnlock;
                if (!rt->isHeapBusy())
                    maybeUnlock.construct(rt);
                ok = rt->gc.pageAllocator.markPagesUnused(aheader->getArena(), ArenaSize);
            }

            if (ok) {
                ++chunk->info.numArenasFree;
                chunk->decommittedArenas.set(arenaIndex);
            } else {
                chunk->addArenaToFreeList(rt, aheader);
            }
            JS_ASSERT(chunk->hasAvailableArenas());
            JS_ASSERT(!chunk->unused());
            if (chunk->info.numArenasFree == 1) {
                /*
                 * Put the chunk back to the available list either at the
                 * point where it was before to preserve the available list
                 * that we enumerate, or, when the allocation thread has fully
                 * used all the previous chunks, at the beginning of the
                 * available list.
                 */
                Chunk **insertPoint = savedPrevp;
                if (savedPrevp != availableListHeadp) {
                    Chunk *prev = Chunk::fromPointerToNext(savedPrevp);
                    if (!prev->hasAvailableArenas())
                        insertPoint = availableListHeadp;
                }
                chunk->insertToAvailableList(insertPoint);
            } else {
                JS_ASSERT(chunk->info.prevp);
            }

            if (rt->gc.chunkAllocationSinceLastGC || !ok) {
                /*
                 * The allocator thread has started to get new chunks. We should stop
                 * to avoid decommitting arenas in just allocated chunks.
                 */
                return;
            }
        }

        /*
         * chunk->info.prevp becomes null when the allocator thread consumed
         * all chunks from the available list.
         */
        JS_ASSERT_IF(chunk->info.prevp, *chunk->info.prevp == chunk);
        if (chunk->info.prevp == availableListHeadp || !chunk->info.prevp)
            break;

        /*
         * prevp exists and is not the list head. It must point to the next
         * field of the previous chunk.
         */
        chunk = chunk->getPrevious();
    }
}

static void
DecommitArenas(JSRuntime *rt)
{
    DecommitArenasFromAvailableList(rt, &rt->gc.systemAvailableChunkListHead);
    DecommitArenasFromAvailableList(rt, &rt->gc.userAvailableChunkListHead);
}

/* Must be called with the GC lock taken. */
static void
ExpireChunksAndArenas(JSRuntime *rt, bool shouldShrink)
{
    if (Chunk *toFree = rt->gc.chunkPool.expire(rt, shouldShrink)) {
        AutoUnlockGC unlock(rt);
        FreeChunkList(rt, toFree);
    }

    if (shouldShrink)
        DecommitArenas(rt);
}

static void
SweepBackgroundThings(JSRuntime* rt, bool onBackgroundThread)
{
    /*
     * We must finalize in the correct order, see comments in
     * finalizeObjects.
     */
    FreeOp fop(rt, false);
    for (int phase = 0 ; phase < BackgroundPhaseCount ; ++phase) {
        for (Zone *zone = rt->gc.sweepingZones; zone; zone = zone->gcNextGraphNode) {
            for (int index = 0 ; index < BackgroundPhaseLength[phase] ; ++index) {
                AllocKind kind = BackgroundPhases[phase][index];
                ArenaHeader *arenas = zone->allocator.arenas.arenaListsToSweep[kind];
                if (arenas)
                    ArenaLists::backgroundFinalize(&fop, arenas, onBackgroundThread);
            }
        }
    }

    rt->gc.sweepingZones = nullptr;
}

#ifdef JS_THREADSAFE
static void
AssertBackgroundSweepingFinished(JSRuntime *rt)
{
    JS_ASSERT(!rt->gc.sweepingZones);
    for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
        for (unsigned i = 0; i < FINALIZE_LIMIT; ++i) {
            JS_ASSERT(!zone->allocator.arenas.arenaListsToSweep[i]);
            JS_ASSERT(zone->allocator.arenas.doneBackgroundFinalize(AllocKind(i)));
        }
    }
}

unsigned
js::GetCPUCount()
{
    static unsigned ncpus = 0;
    if (ncpus == 0) {
# ifdef XP_WIN
        SYSTEM_INFO sysinfo;
        GetSystemInfo(&sysinfo);
        ncpus = unsigned(sysinfo.dwNumberOfProcessors);
# else
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        ncpus = (n > 0) ? unsigned(n) : 1;
# endif
    }
    return ncpus;
}
#endif /* JS_THREADSAFE */

bool
GCHelperThread::init()
{
    if (!rt->useHelperThreads()) {
        backgroundAllocation = false;
        return true;
    }

#ifdef JS_THREADSAFE
    if (!(wakeup = PR_NewCondVar(rt->gc.lock)))
        return false;
    if (!(done = PR_NewCondVar(rt->gc.lock)))
        return false;

    thread = PR_CreateThread(PR_USER_THREAD, threadMain, this, PR_PRIORITY_NORMAL,
                             PR_GLOBAL_THREAD, PR_JOINABLE_THREAD, 0);
    if (!thread)
        return false;

    backgroundAllocation = (GetCPUCount() >= 2);
#endif /* JS_THREADSAFE */
    return true;
}

void
GCHelperThread::finish()
{
    if (!rt->useHelperThreads() || !rt->gc.lock) {
        JS_ASSERT(state == IDLE);
        return;
    }

#ifdef JS_THREADSAFE
    PRThread *join = nullptr;
    {
        AutoLockGC lock(rt);
        if (thread && state != SHUTDOWN) {
            /*
             * We cannot be in the ALLOCATING or CANCEL_ALLOCATION states as
             * the allocations should have been stopped during the last GC.
             */
            JS_ASSERT(state == IDLE || state == SWEEPING);
            if (state == IDLE)
                PR_NotifyCondVar(wakeup);
            state = SHUTDOWN;
            join = thread;
        }
    }
    if (join) {
        /* PR_DestroyThread is not necessary. */
        PR_JoinThread(join);
    }
    if (wakeup)
        PR_DestroyCondVar(wakeup);
    if (done)
        PR_DestroyCondVar(done);
#endif /* JS_THREADSAFE */
}

#ifdef JS_THREADSAFE
#ifdef MOZ_NUWA_PROCESS
extern "C" {
MFBT_API bool IsNuwaProcess();
MFBT_API void NuwaMarkCurrentThread(void (*recreate)(void *), void *arg);
}
#endif

/* static */
void
GCHelperThread::threadMain(void *arg)
{
    PR_SetCurrentThreadName("JS GC Helper");

#ifdef MOZ_NUWA_PROCESS
    if (IsNuwaProcess && IsNuwaProcess()) {
        JS_ASSERT(NuwaMarkCurrentThread != nullptr);
        NuwaMarkCurrentThread(nullptr, nullptr);
    }
#endif

    static_cast<GCHelperThread *>(arg)->threadLoop();
}

void
GCHelperThread::wait(PRCondVar *which)
{
    rt->gc.lockOwner = nullptr;
    PR_WaitCondVar(which, PR_INTERVAL_NO_TIMEOUT);
#ifdef DEBUG
    rt->gc.lockOwner = PR_GetCurrentThread();
#endif
}

void
GCHelperThread::threadLoop()
{
    AutoLockGC lock(rt);

    TraceLogger *logger = TraceLoggerForCurrentThread();

    /*
     * Even on the first iteration the state can be SHUTDOWN or SWEEPING if
     * the stop request or the GC and the corresponding startBackgroundSweep call
     * happen before this thread has a chance to run.
     */
    for (;;) {
        switch (state) {
          case SHUTDOWN:
            return;
          case IDLE:
            wait(wakeup);
            break;
          case SWEEPING: {
            AutoTraceLog logSweeping(logger, TraceLogger::GCSweeping);
            doSweep();
            if (state == SWEEPING)
                state = IDLE;
            PR_NotifyAllCondVar(done);
            break;
          }
          case ALLOCATING: {
            AutoTraceLog logAllocating(logger, TraceLogger::GCAllocation);
            do {
                Chunk *chunk;
                {
                    AutoUnlockGC unlock(rt);
                    chunk = Chunk::allocate(rt);
                }

                /* OOM stops the background allocation. */
                if (!chunk)
                    break;
                JS_ASSERT(chunk->info.numArenasFreeCommitted == 0);
                rt->gc.chunkPool.put(chunk);
            } while (state == ALLOCATING && rt->gc.chunkPool.wantBackgroundAllocation(rt));
            if (state == ALLOCATING)
                state = IDLE;
            break;
          }
          case CANCEL_ALLOCATION:
            state = IDLE;
            PR_NotifyAllCondVar(done);
            break;
        }
    }
}
#endif /* JS_THREADSAFE */

void
GCHelperThread::startBackgroundSweep(bool shouldShrink)
{
    JS_ASSERT(rt->useHelperThreads());

#ifdef JS_THREADSAFE
    AutoLockGC lock(rt);
    JS_ASSERT(state == IDLE);
    JS_ASSERT(!sweepFlag);
    sweepFlag = true;
    shrinkFlag = shouldShrink;
    state = SWEEPING;
    PR_NotifyCondVar(wakeup);
#endif /* JS_THREADSAFE */
}

/* Must be called with the GC lock taken. */
void
GCHelperThread::startBackgroundShrink()
{
    JS_ASSERT(rt->useHelperThreads());

#ifdef JS_THREADSAFE
    switch (state) {
      case IDLE:
        JS_ASSERT(!sweepFlag);
        shrinkFlag = true;
        state = SWEEPING;
        PR_NotifyCondVar(wakeup);
        break;
      case SWEEPING:
        shrinkFlag = true;
        break;
      case ALLOCATING:
      case CANCEL_ALLOCATION:
        /*
         * If we have started background allocation there is nothing to
         * shrink.
         */
        break;
      case SHUTDOWN:
        MOZ_ASSUME_UNREACHABLE("No shrink on shutdown");
    }
#endif /* JS_THREADSAFE */
}

void
GCHelperThread::waitBackgroundSweepEnd()
{
    if (!rt->useHelperThreads()) {
        JS_ASSERT(state == IDLE);
        return;
    }

#ifdef JS_THREADSAFE
    AutoLockGC lock(rt);
    while (state == SWEEPING)
        wait(done);
    if (rt->gc.incrementalState == NO_INCREMENTAL)
        AssertBackgroundSweepingFinished(rt);
#endif /* JS_THREADSAFE */
}

void
GCHelperThread::waitBackgroundSweepOrAllocEnd()
{
    if (!rt->useHelperThreads()) {
        JS_ASSERT(state == IDLE);
        return;
    }

#ifdef JS_THREADSAFE
    AutoLockGC lock(rt);
    if (state == ALLOCATING)
        state = CANCEL_ALLOCATION;
    while (state == SWEEPING || state == CANCEL_ALLOCATION)
        wait(done);
    if (rt->gc.incrementalState == NO_INCREMENTAL)
        AssertBackgroundSweepingFinished(rt);
#endif /* JS_THREADSAFE */
}

/* Must be called with the GC lock taken. */
inline void
GCHelperThread::startBackgroundAllocationIfIdle()
{
    JS_ASSERT(rt->useHelperThreads());

#ifdef JS_THREADSAFE
    if (state == IDLE) {
        state = ALLOCATING;
        PR_NotifyCondVar(wakeup);
    }
#endif /* JS_THREADSAFE */
}

void
GCHelperThread::replenishAndFreeLater(void *ptr)
{
    JS_ASSERT(freeCursor == freeCursorEnd);
    do {
        if (freeCursor && !freeVector.append(freeCursorEnd - FREE_ARRAY_LENGTH))
            break;
        freeCursor = (void **) js_malloc(FREE_ARRAY_SIZE);
        if (!freeCursor) {
            freeCursorEnd = nullptr;
            break;
        }
        freeCursorEnd = freeCursor + FREE_ARRAY_LENGTH;
        *freeCursor++ = ptr;
        return;
    } while (false);
    js_free(ptr);
}

#ifdef JS_THREADSAFE
/* Must be called with the GC lock taken. */
void
GCHelperThread::doSweep()
{
    if (sweepFlag) {
        sweepFlag = false;
        AutoUnlockGC unlock(rt);

        SweepBackgroundThings(rt, true);

        if (freeCursor) {
            void **array = freeCursorEnd - FREE_ARRAY_LENGTH;
            freeElementsAndArray(array, freeCursor);
            freeCursor = freeCursorEnd = nullptr;
        } else {
            JS_ASSERT(!freeCursorEnd);
        }
        for (void ***iter = freeVector.begin(); iter != freeVector.end(); ++iter) {
            void **array = *iter;
            freeElementsAndArray(array, array + FREE_ARRAY_LENGTH);
        }
        freeVector.resize(0);

        rt->freeLifoAlloc.freeAll();
    }

    bool shrinking = shrinkFlag;
    ExpireChunksAndArenas(rt, shrinking);

    /*
     * The main thread may have called ShrinkGCBuffers while
     * ExpireChunksAndArenas(rt, false) was running, so we recheck the flag
     * afterwards.
     */
    if (!shrinking && shrinkFlag) {
        shrinkFlag = false;
        ExpireChunksAndArenas(rt, true);
    }
}
#endif /* JS_THREADSAFE */

bool
GCHelperThread::onBackgroundThread()
{
#ifdef JS_THREADSAFE
    return PR_GetCurrentThread() == getThread();
#else
    return false;
#endif
}

static bool
ReleaseObservedTypes(JSRuntime *rt)
{
    bool releaseTypes = rt->gcZeal() != 0;

#ifndef JS_MORE_DETERMINISTIC
    int64_t now = PRMJ_Now();
    if (now >= rt->gc.jitReleaseTime)
        releaseTypes = true;
    if (releaseTypes)
        rt->gc.jitReleaseTime = now + JIT_SCRIPT_RELEASE_TYPES_INTERVAL;
#endif

    return releaseTypes;
}

/*
 * It's simpler if we preserve the invariant that every zone has at least one
 * compartment. If we know we're deleting the entire zone, then
 * SweepCompartments is allowed to delete all compartments. In this case,
 * |keepAtleastOne| is false. If some objects remain in the zone so that it
 * cannot be deleted, then we set |keepAtleastOne| to true, which prohibits
 * SweepCompartments from deleting every compartment. Instead, it preserves an
 * arbitrary compartment in the zone.
 */
static void
SweepCompartments(FreeOp *fop, Zone *zone, bool keepAtleastOne, bool lastGC)
{
    JSRuntime *rt = zone->runtimeFromMainThread();
    JSDestroyCompartmentCallback callback = rt->destroyCompartmentCallback;

    JSCompartment **read = zone->compartments.begin();
    JSCompartment **end = zone->compartments.end();
    JSCompartment **write = read;
    bool foundOne = false;
    while (read < end) {
        JSCompartment *comp = *read++;
        JS_ASSERT(!rt->isAtomsCompartment(comp));

        /*
         * Don't delete the last compartment if all the ones before it were
         * deleted and keepAtleastOne is true.
         */
        bool dontDelete = read == end && !foundOne && keepAtleastOne;
        if ((!comp->marked && !dontDelete) || lastGC) {
            if (callback)
                callback(fop, comp);
            if (comp->principals)
                JS_DropPrincipals(rt, comp->principals);
            js_delete(comp);
        } else {
            *write++ = comp;
            foundOne = true;
        }
    }
    zone->compartments.resize(write - zone->compartments.begin());
    JS_ASSERT_IF(keepAtleastOne, !zone->compartments.empty());
}

static void
SweepZones(FreeOp *fop, bool lastGC)
{
    JSRuntime *rt = fop->runtime();
    JSZoneCallback callback = rt->destroyZoneCallback;

    /* Skip the atomsCompartment zone. */
    Zone **read = rt->gc.zones.begin() + 1;
    Zone **end = rt->gc.zones.end();
    Zone **write = read;
    JS_ASSERT(rt->gc.zones.length() >= 1);
    JS_ASSERT(rt->isAtomsZone(rt->gc.zones[0]));

    while (read < end) {
        Zone *zone = *read++;

        if (zone->wasGCStarted()) {
            if ((zone->allocator.arenas.arenaListsAreEmpty() && !zone->hasMarkedCompartments()) ||
                lastGC)
            {
                zone->allocator.arenas.checkEmptyFreeLists();
                if (callback)
                    callback(zone);
                SweepCompartments(fop, zone, false, lastGC);
                JS_ASSERT(zone->compartments.empty());
                fop->delete_(zone);
                continue;
            }
            SweepCompartments(fop, zone, true, lastGC);
        }
        *write++ = zone;
    }
    rt->gc.zones.resize(write - rt->gc.zones.begin());
}

static void
PurgeRuntime(JSRuntime *rt)
{
    for (GCCompartmentsIter comp(rt); !comp.done(); comp.next())
        comp->purge();

    rt->freeLifoAlloc.transferUnusedFrom(&rt->tempLifoAlloc);
    rt->interpreterStack().purge(rt);

    rt->gsnCache.purge();
    rt->scopeCoordinateNameCache.purge();
    rt->newObjectCache.purge();
    rt->nativeIterCache.purge();
    rt->sourceDataCache.purge();
    rt->evalCache.clear();

    if (!rt->hasActiveCompilations())
        rt->parseMapPool().purgeAll();
}

static bool
ShouldPreserveJITCode(JSCompartment *comp, int64_t currentTime)
{
    JSRuntime *rt = comp->runtimeFromMainThread();
    if (rt->gc.shouldCleanUpEverything)
        return false;

    if (rt->gc.alwaysPreserveCode)
        return true;
    if (comp->lastAnimationTime + PRMJ_USEC_PER_SEC >= currentTime)
        return true;

    return false;
}

#ifdef DEBUG
class CompartmentCheckTracer : public JSTracer
{
  public:
    CompartmentCheckTracer(JSRuntime *rt, JSTraceCallback callback)
      : JSTracer(rt, callback)
    {}

    Cell *src;
    JSGCTraceKind srcKind;
    Zone *zone;
    JSCompartment *compartment;
};

static bool
InCrossCompartmentMap(JSObject *src, Cell *dst, JSGCTraceKind dstKind)
{
    JSCompartment *srccomp = src->compartment();

    if (dstKind == JSTRACE_OBJECT) {
        Value key = ObjectValue(*static_cast<JSObject *>(dst));
        if (WrapperMap::Ptr p = srccomp->lookupWrapper(key)) {
            if (*p->value().unsafeGet() == ObjectValue(*src))
                return true;
        }
    }

    /*
     * If the cross-compartment edge is caused by the debugger, then we don't
     * know the right hashtable key, so we have to iterate.
     */
    for (JSCompartment::WrapperEnum e(srccomp); !e.empty(); e.popFront()) {
        if (e.front().key().wrapped == dst && ToMarkable(e.front().value()) == src)
            return true;
    }

    return false;
}

static void
CheckCompartment(CompartmentCheckTracer *trc, JSCompartment *thingCompartment,
                 Cell *thing, JSGCTraceKind kind)
{
    JS_ASSERT(thingCompartment == trc->compartment ||
              trc->runtime()->isAtomsCompartment(thingCompartment) ||
              (trc->srcKind == JSTRACE_OBJECT &&
               InCrossCompartmentMap((JSObject *)trc->src, thing, kind)));
}

static JSCompartment *
CompartmentOfCell(Cell *thing, JSGCTraceKind kind)
{
    if (kind == JSTRACE_OBJECT)
        return static_cast<JSObject *>(thing)->compartment();
    else if (kind == JSTRACE_SHAPE)
        return static_cast<Shape *>(thing)->compartment();
    else if (kind == JSTRACE_BASE_SHAPE)
        return static_cast<BaseShape *>(thing)->compartment();
    else if (kind == JSTRACE_SCRIPT)
        return static_cast<JSScript *>(thing)->compartment();
    else
        return nullptr;
}

static void
CheckCompartmentCallback(JSTracer *trcArg, void **thingp, JSGCTraceKind kind)
{
    CompartmentCheckTracer *trc = static_cast<CompartmentCheckTracer *>(trcArg);
    Cell *thing = (Cell *)*thingp;

    JSCompartment *comp = CompartmentOfCell(thing, kind);
    if (comp && trc->compartment) {
        CheckCompartment(trc, comp, thing, kind);
    } else {
        JS_ASSERT(thing->tenuredZone() == trc->zone ||
                  trc->runtime()->isAtomsZone(thing->tenuredZone()));
    }
}

static void
CheckForCompartmentMismatches(JSRuntime *rt)
{
    if (rt->gc.disableStrictProxyCheckingCount)
        return;

    CompartmentCheckTracer trc(rt, CheckCompartmentCallback);
    for (ZonesIter zone(rt, SkipAtoms); !zone.done(); zone.next()) {
        trc.zone = zone;
        for (size_t thingKind = 0; thingKind < FINALIZE_LAST; thingKind++) {
            for (ZoneCellIterUnderGC i(zone, AllocKind(thingKind)); !i.done(); i.next()) {
                trc.src = i.getCell();
                trc.srcKind = MapAllocToTraceKind(AllocKind(thingKind));
                trc.compartment = CompartmentOfCell(trc.src, trc.srcKind);
                JS_TraceChildren(&trc, trc.src, trc.srcKind);
            }
        }
    }
}
#endif

static bool
BeginMarkPhase(JSRuntime *rt)
{
    int64_t currentTime = PRMJ_Now();

#ifdef DEBUG
    if (rt->gc.fullCompartmentChecks)
        CheckForCompartmentMismatches(rt);
#endif

    rt->gc.isFull = true;
    bool any = false;

    for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
        /* Assert that zone state is as we expect */
        JS_ASSERT(!zone->isCollecting());
        JS_ASSERT(!zone->compartments.empty());
        for (unsigned i = 0; i < FINALIZE_LIMIT; ++i)
            JS_ASSERT(!zone->allocator.arenas.arenaListsToSweep[i]);

        /* Set up which zones will be collected. */
        if (zone->isGCScheduled()) {
            if (!rt->isAtomsZone(zone)) {
                any = true;
                zone->setGCState(Zone::Mark);
            }
        } else {
            rt->gc.isFull = false;
        }

        zone->scheduledForDestruction = false;
        zone->maybeAlive = false;
        zone->setPreservingCode(false);
    }

    for (CompartmentsIter c(rt, WithAtoms); !c.done(); c.next()) {
        JS_ASSERT(c->gcLiveArrayBuffers.empty());
        c->marked = false;
        if (ShouldPreserveJITCode(c, currentTime))
            c->zone()->setPreservingCode(true);
    }

    if (!rt->gc.shouldCleanUpEverything) {
#ifdef JS_ION
        if (JSCompartment *comp = jit::TopmostJitActivationCompartment(rt))
            comp->zone()->setPreservingCode(true);
#endif
    }

    /*
     * Atoms are not in the cross-compartment map. So if there are any
     * zones that are not being collected, we are not allowed to collect
     * atoms. Otherwise, the non-collected zones could contain pointers
     * to atoms that we would miss.
     *
     * keepAtoms() will only change on the main thread, which we are currently
     * on. If the value of keepAtoms() changes between GC slices, then we'll
     * cancel the incremental GC. See IsIncrementalGCSafe.
     */
    if (rt->gc.isFull && !rt->keepAtoms()) {
        Zone *atomsZone = rt->atomsCompartment()->zone();
        if (atomsZone->isGCScheduled()) {
            JS_ASSERT(!atomsZone->isCollecting());
            atomsZone->setGCState(Zone::Mark);
            any = true;
        }
    }

    /* Check that at least one zone is scheduled for collection. */
    if (!any)
        return false;

    /*
     * At the end of each incremental slice, we call prepareForIncrementalGC,
     * which marks objects in all arenas that we're currently allocating
     * into. This can cause leaks if unreachable objects are in these
     * arenas. This purge call ensures that we only mark arenas that have had
     * allocations after the incremental GC started.
     */
    if (rt->gc.isIncremental) {
        for (GCZonesIter zone(rt); !zone.done(); zone.next())
            zone->allocator.arenas.purge();
    }

    rt->gc.marker.start();
    JS_ASSERT(!rt->gc.marker.callback);
    JS_ASSERT(IS_GC_MARKING_TRACER(&rt->gc.marker));

    /* For non-incremental GC the following sweep discards the jit code. */
    if (rt->gc.isIncremental) {
        for (GCZonesIter zone(rt); !zone.done(); zone.next()) {
            gcstats::AutoPhase ap(rt->gc.stats, gcstats::PHASE_MARK_DISCARD_CODE);
            zone->discardJitCode(rt->defaultFreeOp());
        }
    }

    GCMarker *gcmarker = &rt->gc.marker;

    rt->gc.startNumber = rt->gc.number;

    /*
     * We must purge the runtime at the beginning of an incremental GC. The
     * danger if we purge later is that the snapshot invariant of incremental
     * GC will be broken, as follows. If some object is reachable only through
     * some cache (say the dtoaCache) then it will not be part of the snapshot.
     * If we purge after root marking, then the mutator could obtain a pointer
     * to the object and start using it. This object might never be marked, so
     * a GC hazard would exist.
     */
    {
        gcstats::AutoPhase ap(rt->gc.stats, gcstats::PHASE_PURGE);
        PurgeRuntime(rt);
    }

    /*
     * Mark phase.
     */
    gcstats::AutoPhase ap1(rt->gc.stats, gcstats::PHASE_MARK);
    gcstats::AutoPhase ap2(rt->gc.stats, gcstats::PHASE_MARK_ROOTS);

    for (GCZonesIter zone(rt); !zone.done(); zone.next()) {
        /* Unmark everything in the zones being collected. */
        zone->allocator.arenas.unmarkAll();
    }

    for (GCCompartmentsIter c(rt); !c.done(); c.next()) {
        /* Reset weak map list for the compartments being collected. */
        WeakMapBase::resetCompartmentWeakMapList(c);
    }

    if (rt->gc.isFull)
        UnmarkScriptData(rt);

    MarkRuntime(gcmarker);
    if (rt->gc.isIncremental)
        BufferGrayRoots(gcmarker);

    /*
     * This code ensures that if a zone is "dead", then it will be
     * collected in this GC. A zone is considered dead if its maybeAlive
     * flag is false. The maybeAlive flag is set if:
     *   (1) the zone has incoming cross-compartment edges, or
     *   (2) an object in the zone was marked during root marking, either
     *       as a black root or a gray root.
     * If the maybeAlive is false, then we set the scheduledForDestruction flag.
     * At any time later in the GC, if we try to mark an object whose
     * zone is scheduled for destruction, we will assert.
     * NOTE: Due to bug 811587, we only assert if gcManipulatingDeadCompartments
     * is true (e.g., if we're doing a brain transplant).
     *
     * The purpose of this check is to ensure that a zone that we would
     * normally destroy is not resurrected by a read barrier or an
     * allocation. This might happen during a function like JS_TransplantObject,
     * which iterates over all compartments, live or dead, and operates on their
     * objects. See bug 803376 for details on this problem. To avoid the
     * problem, we are very careful to avoid allocation and read barriers during
     * JS_TransplantObject and the like. The code here ensures that we don't
     * regress.
     *
     * Note that there are certain cases where allocations or read barriers in
     * dead zone are difficult to avoid. We detect such cases (via the
     * gcObjectsMarkedInDeadCompartment counter) and redo any ongoing GCs after
     * the JS_TransplantObject function has finished. This ensures that the dead
     * zones will be cleaned up. See AutoMarkInDeadZone and
     * AutoMaybeTouchDeadZones for details.
     */

    /* Set the maybeAlive flag based on cross-compartment edges. */
    for (CompartmentsIter c(rt, SkipAtoms); !c.done(); c.next()) {
        for (JSCompartment::WrapperEnum e(c); !e.empty(); e.popFront()) {
            Cell *dst = e.front().key().wrapped;
            dst->tenuredZone()->maybeAlive = true;
        }
    }

    /*
     * For black roots, code in gc/Marking.cpp will already have set maybeAlive
     * during MarkRuntime.
     */

    for (GCZonesIter zone(rt); !zone.done(); zone.next()) {
        if (!zone->maybeAlive && !rt->isAtomsZone(zone))
            zone->scheduledForDestruction = true;
    }
    rt->gc.foundBlackGrayEdges = false;

    return true;
}

template <class CompartmentIterT>
static void
MarkWeakReferences(JSRuntime *rt, gcstats::Phase phase)
{
    GCMarker *gcmarker = &rt->gc.marker;
    JS_ASSERT(gcmarker->isDrained());

    gcstats::AutoPhase ap(rt->gc.stats, gcstats::PHASE_SWEEP_MARK);
    gcstats::AutoPhase ap1(rt->gc.stats, phase);

    for (;;) {
        bool markedAny = false;
        for (CompartmentIterT c(rt); !c.done(); c.next()) {
            markedAny |= WatchpointMap::markCompartmentIteratively(c, gcmarker);
            markedAny |= WeakMapBase::markCompartmentIteratively(c, gcmarker);
        }
        markedAny |= Debugger::markAllIteratively(gcmarker);

        if (!markedAny)
            break;

        SliceBudget budget;
        gcmarker->drainMarkStack(budget);
    }
    JS_ASSERT(gcmarker->isDrained());
}

static void
MarkWeakReferencesInCurrentGroup(JSRuntime *rt, gcstats::Phase phase)
{
    MarkWeakReferences<GCCompartmentGroupIter>(rt, phase);
}

template <class ZoneIterT, class CompartmentIterT>
static void
MarkGrayReferences(JSRuntime *rt)
{
    GCMarker *gcmarker = &rt->gc.marker;

    {
        gcstats::AutoPhase ap(rt->gc.stats, gcstats::PHASE_SWEEP_MARK);
        gcstats::AutoPhase ap1(rt->gc.stats, gcstats::PHASE_SWEEP_MARK_GRAY);
        gcmarker->setMarkColorGray();
        if (gcmarker->hasBufferedGrayRoots()) {
            for (ZoneIterT zone(rt); !zone.done(); zone.next())
                gcmarker->markBufferedGrayRoots(zone);
        } else {
            JS_ASSERT(!rt->gc.isIncremental);
            if (JSTraceDataOp op = rt->gc.grayRootTracer.op)
                (*op)(gcmarker, rt->gc.grayRootTracer.data);
        }
        SliceBudget budget;
        gcmarker->drainMarkStack(budget);
    }

    MarkWeakReferences<CompartmentIterT>(rt, gcstats::PHASE_SWEEP_MARK_GRAY_WEAK);

    JS_ASSERT(gcmarker->isDrained());

    gcmarker->setMarkColorBlack();
}

static void
MarkGrayReferencesInCurrentGroup(JSRuntime *rt)
{
    MarkGrayReferences<GCZoneGroupIter, GCCompartmentGroupIter>(rt);
}

#ifdef DEBUG

static void
MarkAllWeakReferences(JSRuntime *rt, gcstats::Phase phase)
{
    MarkWeakReferences<GCCompartmentsIter>(rt, phase);
}

static void
MarkAllGrayReferences(JSRuntime *rt)
{
    MarkGrayReferences<GCZonesIter, GCCompartmentsIter>(rt);
}

class js::gc::MarkingValidator
{
  public:
    MarkingValidator(JSRuntime *rt);
    ~MarkingValidator();
    void nonIncrementalMark();
    void validate();

  private:
    JSRuntime *runtime;
    bool initialized;

    typedef HashMap<Chunk *, ChunkBitmap *, GCChunkHasher, SystemAllocPolicy> BitmapMap;
    BitmapMap map;
};

js::gc::MarkingValidator::MarkingValidator(JSRuntime *rt)
  : runtime(rt),
    initialized(false)
{}

js::gc::MarkingValidator::~MarkingValidator()
{
    if (!map.initialized())
        return;

    for (BitmapMap::Range r(map.all()); !r.empty(); r.popFront())
        js_delete(r.front().value());
}

void
js::gc::MarkingValidator::nonIncrementalMark()
{
    /*
     * Perform a non-incremental mark for all collecting zones and record
     * the results for later comparison.
     *
     * Currently this does not validate gray marking.
     */

    if (!map.init())
        return;

    GCMarker *gcmarker = &runtime->gc.marker;

    /* Save existing mark bits. */
    for (GCChunkSet::Range r(runtime->gc.chunkSet.all()); !r.empty(); r.popFront()) {
        ChunkBitmap *bitmap = &r.front()->bitmap;
	ChunkBitmap *entry = js_new<ChunkBitmap>();
        if (!entry)
            return;

        memcpy((void *)entry->bitmap, (void *)bitmap->bitmap, sizeof(bitmap->bitmap));
        if (!map.putNew(r.front(), entry))
            return;
    }

    /*
     * Temporarily clear the lists of live weakmaps and array buffers for the
     * compartments we are collecting.
     */

    WeakMapVector weakmaps;
    ArrayBufferVector arrayBuffers;
    for (GCCompartmentsIter c(runtime); !c.done(); c.next()) {
        if (!WeakMapBase::saveCompartmentWeakMapList(c, weakmaps) ||
            !ArrayBufferObject::saveArrayBufferList(c, arrayBuffers))
        {
            return;
        }
    }

    /*
     * After this point, the function should run to completion, so we shouldn't
     * do anything fallible.
     */
    initialized = true;

    for (GCCompartmentsIter c(runtime); !c.done(); c.next()) {
        WeakMapBase::resetCompartmentWeakMapList(c);
        ArrayBufferObject::resetArrayBufferList(c);
    }

    /* Re-do all the marking, but non-incrementally. */
    js::gc::State state = runtime->gc.incrementalState;
    runtime->gc.incrementalState = MARK_ROOTS;

    JS_ASSERT(gcmarker->isDrained());
    gcmarker->reset();

    for (GCChunkSet::Range r(runtime->gc.chunkSet.all()); !r.empty(); r.popFront())
        r.front()->bitmap.clear();

    {
        gcstats::AutoPhase ap1(runtime->gc.stats, gcstats::PHASE_MARK);
        gcstats::AutoPhase ap2(runtime->gc.stats, gcstats::PHASE_MARK_ROOTS);
        MarkRuntime(gcmarker, true);
    }

    {
        gcstats::AutoPhase ap1(runtime->gc.stats, gcstats::PHASE_MARK);
        SliceBudget budget;
        runtime->gc.incrementalState = MARK;
        runtime->gc.marker.drainMarkStack(budget);
    }

    runtime->gc.incrementalState = SWEEP;
    {
        gcstats::AutoPhase ap(runtime->gc.stats, gcstats::PHASE_SWEEP);
        MarkAllWeakReferences(runtime, gcstats::PHASE_SWEEP_MARK_WEAK);

        /* Update zone state for gray marking. */
        for (GCZonesIter zone(runtime); !zone.done(); zone.next()) {
            JS_ASSERT(zone->isGCMarkingBlack());
            zone->setGCState(Zone::MarkGray);
        }

        MarkAllGrayReferences(runtime);

        /* Restore zone state. */
        for (GCZonesIter zone(runtime); !zone.done(); zone.next()) {
            JS_ASSERT(zone->isGCMarkingGray());
            zone->setGCState(Zone::Mark);
        }
    }

    /* Take a copy of the non-incremental mark state and restore the original. */
    for (GCChunkSet::Range r(runtime->gc.chunkSet.all()); !r.empty(); r.popFront()) {
        Chunk *chunk = r.front();
        ChunkBitmap *bitmap = &chunk->bitmap;
        ChunkBitmap *entry = map.lookup(chunk)->value();
        Swap(*entry, *bitmap);
    }

    for (GCCompartmentsIter c(runtime); !c.done(); c.next()) {
        WeakMapBase::resetCompartmentWeakMapList(c);
        ArrayBufferObject::resetArrayBufferList(c);
    }
    WeakMapBase::restoreCompartmentWeakMapLists(weakmaps);
    ArrayBufferObject::restoreArrayBufferLists(arrayBuffers);

    runtime->gc.incrementalState = state;
}

void
js::gc::MarkingValidator::validate()
{
    /*
     * Validates the incremental marking for a single compartment by comparing
     * the mark bits to those previously recorded for a non-incremental mark.
     */

    if (!initialized)
        return;

    for (GCChunkSet::Range r(runtime->gc.chunkSet.all()); !r.empty(); r.popFront()) {
        Chunk *chunk = r.front();
        BitmapMap::Ptr ptr = map.lookup(chunk);
        if (!ptr)
            continue;  /* Allocated after we did the non-incremental mark. */

        ChunkBitmap *bitmap = ptr->value();
        ChunkBitmap *incBitmap = &chunk->bitmap;

        for (size_t i = 0; i < ArenasPerChunk; i++) {
            if (chunk->decommittedArenas.get(i))
                continue;
            Arena *arena = &chunk->arenas[i];
            if (!arena->aheader.allocated())
                continue;
            if (!arena->aheader.zone->isGCSweeping())
                continue;
            if (arena->aheader.allocatedDuringIncremental)
                continue;

            AllocKind kind = arena->aheader.getAllocKind();
            uintptr_t thing = arena->thingsStart(kind);
            uintptr_t end = arena->thingsEnd();
            while (thing < end) {
                Cell *cell = (Cell *)thing;

                /*
                 * If a non-incremental GC wouldn't have collected a cell, then
                 * an incremental GC won't collect it.
                 */
                JS_ASSERT_IF(bitmap->isMarked(cell, BLACK), incBitmap->isMarked(cell, BLACK));

                /*
                 * If the cycle collector isn't allowed to collect an object
                 * after a non-incremental GC has run, then it isn't allowed to
                 * collected it after an incremental GC.
                 */
                JS_ASSERT_IF(!bitmap->isMarked(cell, GRAY), !incBitmap->isMarked(cell, GRAY));

                thing += Arena::thingSize(kind);
            }
        }
    }
}

#endif

static void
ComputeNonIncrementalMarkingForValidation(JSRuntime *rt)
{
#ifdef DEBUG
    JS_ASSERT(!rt->gc.markingValidator);
    if (rt->gc.isIncremental && rt->gc.validate)
        rt->gc.markingValidator = js_new<MarkingValidator>(rt);
    if (rt->gc.markingValidator)
        rt->gc.markingValidator->nonIncrementalMark();
#endif
}

static void
ValidateIncrementalMarking(JSRuntime *rt)
{
#ifdef DEBUG
    if (rt->gc.markingValidator)
        rt->gc.markingValidator->validate();
#endif
}

static void
FinishMarkingValidation(JSRuntime *rt)
{
#ifdef DEBUG
    js_delete(rt->gc.markingValidator);
    rt->gc.markingValidator = nullptr;
#endif
}

static void
AssertNeedsBarrierFlagsConsistent(JSRuntime *rt)
{
#ifdef DEBUG
    bool anyNeedsBarrier = false;
    for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next())
        anyNeedsBarrier |= zone->needsBarrier();
    JS_ASSERT(rt->needsBarrier() == anyNeedsBarrier);
#endif
}

static void
DropStringWrappers(JSRuntime *rt)
{
    /*
     * String "wrappers" are dropped on GC because their presence would require
     * us to sweep the wrappers in all compartments every time we sweep a
     * compartment group.
     */
    for (CompartmentsIter c(rt, SkipAtoms); !c.done(); c.next()) {
        for (JSCompartment::WrapperEnum e(c); !e.empty(); e.popFront()) {
            if (e.front().key().kind == CrossCompartmentKey::StringWrapper)
                e.removeFront();
        }
    }
}

/*
 * Group zones that must be swept at the same time.
 *
 * If compartment A has an edge to an unmarked object in compartment B, then we
 * must not sweep A in a later slice than we sweep B. That's because a write
 * barrier in A that could lead to the unmarked object in B becoming
 * marked. However, if we had already swept that object, we would be in trouble.
 *
 * If we consider these dependencies as a graph, then all the compartments in
 * any strongly-connected component of this graph must be swept in the same
 * slice.
 *
 * Tarjan's algorithm is used to calculate the components.
 */

void
JSCompartment::findOutgoingEdges(ComponentFinder<JS::Zone> &finder)
{
    for (js::WrapperMap::Enum e(crossCompartmentWrappers); !e.empty(); e.popFront()) {
        CrossCompartmentKey::Kind kind = e.front().key().kind;
        JS_ASSERT(kind != CrossCompartmentKey::StringWrapper);
        Cell *other = e.front().key().wrapped;
        if (kind == CrossCompartmentKey::ObjectWrapper) {
            /*
             * Add edge to wrapped object compartment if wrapped object is not
             * marked black to indicate that wrapper compartment not be swept
             * after wrapped compartment.
             */
            if (!other->isMarked(BLACK) || other->isMarked(GRAY)) {
                JS::Zone *w = other->tenuredZone();
                if (w->isGCMarking())
                    finder.addEdgeTo(w);
            }
        } else {
            JS_ASSERT(kind == CrossCompartmentKey::DebuggerScript ||
                      kind == CrossCompartmentKey::DebuggerSource ||
                      kind == CrossCompartmentKey::DebuggerObject ||
                      kind == CrossCompartmentKey::DebuggerEnvironment);
            /*
             * Add edge for debugger object wrappers, to ensure (in conjuction
             * with call to Debugger::findCompartmentEdges below) that debugger
             * and debuggee objects are always swept in the same group.
             */
            JS::Zone *w = other->tenuredZone();
            if (w->isGCMarking())
                finder.addEdgeTo(w);
        }
    }

    Debugger::findCompartmentEdges(zone(), finder);
}

void
Zone::findOutgoingEdges(ComponentFinder<JS::Zone> &finder)
{
    /*
     * Any compartment may have a pointer to an atom in the atoms
     * compartment, and these aren't in the cross compartment map.
     */
    JSRuntime *rt = runtimeFromMainThread();
    if (rt->atomsCompartment()->zone()->isGCMarking())
        finder.addEdgeTo(rt->atomsCompartment()->zone());

    for (CompartmentsInZoneIter comp(this); !comp.done(); comp.next())
        comp->findOutgoingEdges(finder);
}

static void
FindZoneGroups(JSRuntime *rt)
{
    ComponentFinder<Zone> finder(rt->mainThread.nativeStackLimit[StackForSystemCode]);
    if (!rt->gc.isIncremental)
        finder.useOneComponent();

    for (GCZonesIter zone(rt); !zone.done(); zone.next()) {
        JS_ASSERT(zone->isGCMarking());
        finder.addNode(zone);
    }
    rt->gc.zoneGroups = finder.getResultsList();
    rt->gc.currentZoneGroup = rt->gc.zoneGroups;
    rt->gc.zoneGroupIndex = 0;
    JS_ASSERT_IF(!rt->gc.isIncremental, !rt->gc.currentZoneGroup->nextGroup());
}

static void
ResetGrayList(JSCompartment* comp);

static void
GetNextZoneGroup(JSRuntime *rt)
{
    rt->gc.currentZoneGroup = rt->gc.currentZoneGroup->nextGroup();
    ++rt->gc.zoneGroupIndex;
    if (!rt->gc.currentZoneGroup) {
        rt->gc.abortSweepAfterCurrentGroup = false;
        return;
    }

    if (!rt->gc.isIncremental)
        ComponentFinder<Zone>::mergeGroups(rt->gc.currentZoneGroup);

    if (rt->gc.abortSweepAfterCurrentGroup) {
        JS_ASSERT(!rt->gc.isIncremental);
        for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
            JS_ASSERT(!zone->gcNextGraphComponent);
            JS_ASSERT(zone->isGCMarking());
            zone->setNeedsBarrier(false, Zone::UpdateIon);
            zone->setGCState(Zone::NoGC);
            zone->gcGrayRoots.clearAndFree();
        }
        rt->setNeedsBarrier(false);
        AssertNeedsBarrierFlagsConsistent(rt);

        for (GCCompartmentGroupIter comp(rt); !comp.done(); comp.next()) {
            ArrayBufferObject::resetArrayBufferList(comp);
            ResetGrayList(comp);
        }

        rt->gc.abortSweepAfterCurrentGroup = false;
        rt->gc.currentZoneGroup = nullptr;
    }
}

/*
 * Gray marking:
 *
 * At the end of collection, anything reachable from a gray root that has not
 * otherwise been marked black must be marked gray.
 *
 * This means that when marking things gray we must not allow marking to leave
 * the current compartment group, as that could result in things being marked
 * grey when they might subsequently be marked black.  To achieve this, when we
 * find a cross compartment pointer we don't mark the referent but add it to a
 * singly-linked list of incoming gray pointers that is stored with each
 * compartment.
 *
 * The list head is stored in JSCompartment::gcIncomingGrayPointers and contains
 * cross compartment wrapper objects. The next pointer is stored in the second
 * extra slot of the cross compartment wrapper.
 *
 * The list is created during gray marking when one of the
 * MarkCrossCompartmentXXX functions is called for a pointer that leaves the
 * current compartent group.  This calls DelayCrossCompartmentGrayMarking to
 * push the referring object onto the list.
 *
 * The list is traversed and then unlinked in
 * MarkIncomingCrossCompartmentPointers.
 */

static bool
IsGrayListObject(JSObject *obj)
{
    JS_ASSERT(obj);
    return obj->is<CrossCompartmentWrapperObject>() && !IsDeadProxyObject(obj);
}

/* static */ unsigned
ProxyObject::grayLinkSlot(JSObject *obj)
{
    JS_ASSERT(IsGrayListObject(obj));
    return ProxyObject::EXTRA_SLOT + 1;
}

#ifdef DEBUG
static void
AssertNotOnGrayList(JSObject *obj)
{
    JS_ASSERT_IF(IsGrayListObject(obj),
                 obj->getReservedSlot(ProxyObject::grayLinkSlot(obj)).isUndefined());
}
#endif

static JSObject *
CrossCompartmentPointerReferent(JSObject *obj)
{
    JS_ASSERT(IsGrayListObject(obj));
    return &obj->as<ProxyObject>().private_().toObject();
}

static JSObject *
NextIncomingCrossCompartmentPointer(JSObject *prev, bool unlink)
{
    unsigned slot = ProxyObject::grayLinkSlot(prev);
    JSObject *next = prev->getReservedSlot(slot).toObjectOrNull();
    JS_ASSERT_IF(next, IsGrayListObject(next));

    if (unlink)
        prev->setSlot(slot, UndefinedValue());

    return next;
}

void
js::DelayCrossCompartmentGrayMarking(JSObject *src)
{
    JS_ASSERT(IsGrayListObject(src));

    /* Called from MarkCrossCompartmentXXX functions. */
    unsigned slot = ProxyObject::grayLinkSlot(src);
    JSObject *dest = CrossCompartmentPointerReferent(src);
    JSCompartment *comp = dest->compartment();

    if (src->getReservedSlot(slot).isUndefined()) {
        src->setCrossCompartmentSlot(slot, ObjectOrNullValue(comp->gcIncomingGrayPointers));
        comp->gcIncomingGrayPointers = src;
    } else {
        JS_ASSERT(src->getReservedSlot(slot).isObjectOrNull());
    }

#ifdef DEBUG
    /*
     * Assert that the object is in our list, also walking the list to check its
     * integrity.
     */
    JSObject *obj = comp->gcIncomingGrayPointers;
    bool found = false;
    while (obj) {
        if (obj == src)
            found = true;
        obj = NextIncomingCrossCompartmentPointer(obj, false);
    }
    JS_ASSERT(found);
#endif
}

static void
MarkIncomingCrossCompartmentPointers(JSRuntime *rt, const uint32_t color)
{
    JS_ASSERT(color == BLACK || color == GRAY);

    gcstats::AutoPhase ap(rt->gc.stats, gcstats::PHASE_SWEEP_MARK);
    static const gcstats::Phase statsPhases[] = {
        gcstats::PHASE_SWEEP_MARK_INCOMING_BLACK,
        gcstats::PHASE_SWEEP_MARK_INCOMING_GRAY
    };
    gcstats::AutoPhase ap1(rt->gc.stats, statsPhases[color]);

    bool unlinkList = color == GRAY;

    for (GCCompartmentGroupIter c(rt); !c.done(); c.next()) {
        JS_ASSERT_IF(color == GRAY, c->zone()->isGCMarkingGray());
        JS_ASSERT_IF(color == BLACK, c->zone()->isGCMarkingBlack());
        JS_ASSERT_IF(c->gcIncomingGrayPointers, IsGrayListObject(c->gcIncomingGrayPointers));

        for (JSObject *src = c->gcIncomingGrayPointers;
             src;
             src = NextIncomingCrossCompartmentPointer(src, unlinkList))
        {
            JSObject *dst = CrossCompartmentPointerReferent(src);
            JS_ASSERT(dst->compartment() == c);

            if (color == GRAY) {
                if (IsObjectMarked(&src) && src->isMarked(GRAY))
                    MarkGCThingUnbarriered(&rt->gc.marker, (void**)&dst,
                                           "cross-compartment gray pointer");
            } else {
                if (IsObjectMarked(&src) && !src->isMarked(GRAY))
                    MarkGCThingUnbarriered(&rt->gc.marker, (void**)&dst,
                                           "cross-compartment black pointer");
            }
        }

        if (unlinkList)
            c->gcIncomingGrayPointers = nullptr;
    }

    SliceBudget budget;
    rt->gc.marker.drainMarkStack(budget);
}

static bool
RemoveFromGrayList(JSObject *wrapper)
{
    if (!IsGrayListObject(wrapper))
        return false;

    unsigned slot = ProxyObject::grayLinkSlot(wrapper);
    if (wrapper->getReservedSlot(slot).isUndefined())
        return false;  /* Not on our list. */

    JSObject *tail = wrapper->getReservedSlot(slot).toObjectOrNull();
    wrapper->setReservedSlot(slot, UndefinedValue());

    JSCompartment *comp = CrossCompartmentPointerReferent(wrapper)->compartment();
    JSObject *obj = comp->gcIncomingGrayPointers;
    if (obj == wrapper) {
        comp->gcIncomingGrayPointers = tail;
        return true;
    }

    while (obj) {
        unsigned slot = ProxyObject::grayLinkSlot(obj);
        JSObject *next = obj->getReservedSlot(slot).toObjectOrNull();
        if (next == wrapper) {
            obj->setCrossCompartmentSlot(slot, ObjectOrNullValue(tail));
            return true;
        }
        obj = next;
    }

    MOZ_ASSUME_UNREACHABLE("object not found in gray link list");
}

static void
ResetGrayList(JSCompartment *comp)
{
    JSObject *src = comp->gcIncomingGrayPointers;
    while (src)
        src = NextIncomingCrossCompartmentPointer(src, true);
    comp->gcIncomingGrayPointers = nullptr;
}

void
js::NotifyGCNukeWrapper(JSObject *obj)
{
    /*
     * References to target of wrapper are being removed, we no longer have to
     * remember to mark it.
     */
    RemoveFromGrayList(obj);
}

enum {
    JS_GC_SWAP_OBJECT_A_REMOVED = 1 << 0,
    JS_GC_SWAP_OBJECT_B_REMOVED = 1 << 1
};

unsigned
js::NotifyGCPreSwap(JSObject *a, JSObject *b)
{
    /*
     * Two objects in the same compartment are about to have had their contents
     * swapped.  If either of them are in our gray pointer list, then we remove
     * them from the lists, returning a bitset indicating what happened.
     */
    return (RemoveFromGrayList(a) ? JS_GC_SWAP_OBJECT_A_REMOVED : 0) |
           (RemoveFromGrayList(b) ? JS_GC_SWAP_OBJECT_B_REMOVED : 0);
}

void
js::NotifyGCPostSwap(JSObject *a, JSObject *b, unsigned removedFlags)
{
    /*
     * Two objects in the same compartment have had their contents swapped.  If
     * either of them were in our gray pointer list, we re-add them again.
     */
    if (removedFlags & JS_GC_SWAP_OBJECT_A_REMOVED)
        DelayCrossCompartmentGrayMarking(b);
    if (removedFlags & JS_GC_SWAP_OBJECT_B_REMOVED)
        DelayCrossCompartmentGrayMarking(a);
}

static void
EndMarkingZoneGroup(JSRuntime *rt)
{
    /*
     * Mark any incoming black pointers from previously swept compartments
     * whose referents are not marked. This can occur when gray cells become
     * black by the action of UnmarkGray.
     */
    MarkIncomingCrossCompartmentPointers(rt, BLACK);

    MarkWeakReferencesInCurrentGroup(rt, gcstats::PHASE_SWEEP_MARK_WEAK);

    /*
     * Change state of current group to MarkGray to restrict marking to this
     * group.  Note that there may be pointers to the atoms compartment, and
     * these will be marked through, as they are not marked with
     * MarkCrossCompartmentXXX.
     */
    for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
        JS_ASSERT(zone->isGCMarkingBlack());
        zone->setGCState(Zone::MarkGray);
    }

    /* Mark incoming gray pointers from previously swept compartments. */
    rt->gc.marker.setMarkColorGray();
    MarkIncomingCrossCompartmentPointers(rt, GRAY);
    rt->gc.marker.setMarkColorBlack();

    /* Mark gray roots and mark transitively inside the current compartment group. */
    MarkGrayReferencesInCurrentGroup(rt);

    /* Restore marking state. */
    for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
        JS_ASSERT(zone->isGCMarkingGray());
        zone->setGCState(Zone::Mark);
    }

    JS_ASSERT(rt->gc.marker.isDrained());
}

static void
BeginSweepingZoneGroup(JSRuntime *rt)
{
    /*
     * Begin sweeping the group of zones in gcCurrentZoneGroup,
     * performing actions that must be done before yielding to caller.
     */

    bool sweepingAtoms = false;
    for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
        /* Set the GC state to sweeping. */
        JS_ASSERT(zone->isGCMarking());
        zone->setGCState(Zone::Sweep);

        /* Purge the ArenaLists before sweeping. */
        zone->allocator.arenas.purge();

        if (rt->isAtomsZone(zone))
            sweepingAtoms = true;

        if (rt->sweepZoneCallback)
            rt->sweepZoneCallback(zone);
    }

    ValidateIncrementalMarking(rt);

    FreeOp fop(rt, rt->gc.sweepOnBackgroundThread);

    {
        gcstats::AutoPhase ap(rt->gc.stats, gcstats::PHASE_FINALIZE_START);
        if (rt->gc.finalizeCallback)
            rt->gc.finalizeCallback(&fop, JSFINALIZE_GROUP_START, !rt->gc.isFull /* unused */);
    }

    if (sweepingAtoms) {
        gcstats::AutoPhase ap(rt->gc.stats, gcstats::PHASE_SWEEP_ATOMS);
        rt->sweepAtoms();
    }

    /* Prune out dead views from ArrayBuffer's view lists. */
    for (GCCompartmentGroupIter c(rt); !c.done(); c.next())
        ArrayBufferObject::sweep(c);

    /* Collect watch points associated with unreachable objects. */
    WatchpointMap::sweepAll(rt);

    /* Detach unreachable debuggers and global objects from each other. */
    Debugger::sweepAll(&fop);

    {
        gcstats::AutoPhase ap(rt->gc.stats, gcstats::PHASE_SWEEP_COMPARTMENTS);

        for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
            gcstats::AutoPhase ap(rt->gc.stats, gcstats::PHASE_SWEEP_DISCARD_CODE);
            zone->discardJitCode(&fop);
        }

        bool releaseTypes = ReleaseObservedTypes(rt);
        for (GCCompartmentGroupIter c(rt); !c.done(); c.next()) {
            gcstats::AutoSCC scc(rt->gc.stats, rt->gc.zoneGroupIndex);
            c->sweep(&fop, releaseTypes && !c->zone()->isPreservingCode());
        }

        for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
            gcstats::AutoSCC scc(rt->gc.stats, rt->gc.zoneGroupIndex);

            // If there is an OOM while sweeping types, the type information
            // will be deoptimized so that it is still correct (i.e.
            // overapproximates the possible types in the zone), but the
            // constraints might not have been triggered on the deoptimization
            // or even copied over completely. In this case, destroy all JIT
            // code and new script addendums in the zone, the only things whose
            // correctness depends on the type constraints.
            bool oom = false;
            zone->sweep(&fop, releaseTypes && !zone->isPreservingCode(), &oom);

            if (oom) {
                zone->setPreservingCode(false);
                zone->discardJitCode(&fop);
                zone->types.clearAllNewScriptAddendumsOnOOM();
            }
        }
    }

    /*
     * Queue all GC things in all zones for sweeping, either in the
     * foreground or on the background thread.
     *
     * Note that order is important here for the background case.
     *
     * Objects are finalized immediately but this may change in the future.
     */
    for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
        gcstats::AutoSCC scc(rt->gc.stats, rt->gc.zoneGroupIndex);
        zone->allocator.arenas.queueObjectsForSweep(&fop);
    }
    for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
        gcstats::AutoSCC scc(rt->gc.stats, rt->gc.zoneGroupIndex);
        zone->allocator.arenas.queueStringsForSweep(&fop);
    }
    for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
        gcstats::AutoSCC scc(rt->gc.stats, rt->gc.zoneGroupIndex);
        zone->allocator.arenas.queueScriptsForSweep(&fop);
    }
#ifdef JS_ION
    for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
        gcstats::AutoSCC scc(rt->gc.stats, rt->gc.zoneGroupIndex);
        zone->allocator.arenas.queueJitCodeForSweep(&fop);
    }
#endif
    for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
        gcstats::AutoSCC scc(rt->gc.stats, rt->gc.zoneGroupIndex);
        zone->allocator.arenas.queueShapesForSweep(&fop);
        zone->allocator.arenas.gcShapeArenasToSweep =
            zone->allocator.arenas.arenaListsToSweep[FINALIZE_SHAPE];
    }

    rt->gc.sweepPhase = 0;
    rt->gc.sweepZone = rt->gc.currentZoneGroup;
    rt->gc.sweepKindIndex = 0;

    {
        gcstats::AutoPhase ap(rt->gc.stats, gcstats::PHASE_FINALIZE_END);
        if (rt->gc.finalizeCallback)
            rt->gc.finalizeCallback(&fop, JSFINALIZE_GROUP_END, !rt->gc.isFull /* unused */);
    }
}

static void
EndSweepingZoneGroup(JSRuntime *rt)
{
    /* Update the GC state for zones we have swept and unlink the list. */
    for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
        JS_ASSERT(zone->isGCSweeping());
        zone->setGCState(Zone::Finished);
    }

    /* Reset the list of arenas marked as being allocated during sweep phase. */
    while (ArenaHeader *arena = rt->gc.arenasAllocatedDuringSweep) {
        rt->gc.arenasAllocatedDuringSweep = arena->getNextAllocDuringSweep();
        arena->unsetAllocDuringSweep();
    }
}

static void
BeginSweepPhase(JSRuntime *rt, bool lastGC)
{
    /*
     * Sweep phase.
     *
     * Finalize as we sweep, outside of rt->gc.lock but with rt->isHeapBusy()
     * true so that any attempt to allocate a GC-thing from a finalizer will
     * fail, rather than nest badly and leave the unmarked newborn to be swept.
     */

    JS_ASSERT(!rt->gc.abortSweepAfterCurrentGroup);

    ComputeNonIncrementalMarkingForValidation(rt);

    gcstats::AutoPhase ap(rt->gc.stats, gcstats::PHASE_SWEEP);

#ifdef JS_THREADSAFE
    rt->gc.sweepOnBackgroundThread = !lastGC && rt->useHelperThreads();
#endif

#ifdef DEBUG
    for (CompartmentsIter c(rt, SkipAtoms); !c.done(); c.next()) {
        JS_ASSERT(!c->gcIncomingGrayPointers);
        for (JSCompartment::WrapperEnum e(c); !e.empty(); e.popFront()) {
            if (e.front().key().kind != CrossCompartmentKey::StringWrapper)
                AssertNotOnGrayList(&e.front().value().get().toObject());
        }
    }
#endif

    DropStringWrappers(rt);
    FindZoneGroups(rt);
    EndMarkingZoneGroup(rt);
    BeginSweepingZoneGroup(rt);
}

bool
ArenaLists::foregroundFinalize(FreeOp *fop, AllocKind thingKind, SliceBudget &sliceBudget)
{
    if (!arenaListsToSweep[thingKind])
        return true;

    ArenaList &dest = arenaLists[thingKind];
    return FinalizeArenas(fop, &arenaListsToSweep[thingKind], dest, thingKind, sliceBudget);
}

static bool
DrainMarkStack(JSRuntime *rt, SliceBudget &sliceBudget, gcstats::Phase phase)
{
    /* Run a marking slice and return whether the stack is now empty. */
    gcstats::AutoPhase ap(rt->gc.stats, phase);
    return rt->gc.marker.drainMarkStack(sliceBudget);
}

static bool
SweepPhase(JSRuntime *rt, SliceBudget &sliceBudget)
{
    gcstats::AutoPhase ap(rt->gc.stats, gcstats::PHASE_SWEEP);
    FreeOp fop(rt, rt->gc.sweepOnBackgroundThread);

    bool finished = DrainMarkStack(rt, sliceBudget, gcstats::PHASE_SWEEP_MARK);
    if (!finished)
        return false;

    for (;;) {
        /* Finalize foreground finalized things. */
        for (; rt->gc.sweepPhase < FinalizePhaseCount ; ++rt->gc.sweepPhase) {
            gcstats::AutoPhase ap(rt->gc.stats, FinalizePhaseStatsPhase[rt->gc.sweepPhase]);

            for (; rt->gc.sweepZone; rt->gc.sweepZone = rt->gc.sweepZone->nextNodeInGroup()) {
                Zone *zone = rt->gc.sweepZone;

                while (rt->gc.sweepKindIndex < FinalizePhaseLength[rt->gc.sweepPhase]) {
                    AllocKind kind = FinalizePhases[rt->gc.sweepPhase][rt->gc.sweepKindIndex];

                    if (!zone->allocator.arenas.foregroundFinalize(&fop, kind, sliceBudget))
                        return false;  /* Yield to the mutator. */

                    ++rt->gc.sweepKindIndex;
                }
                rt->gc.sweepKindIndex = 0;
            }
            rt->gc.sweepZone = rt->gc.currentZoneGroup;
        }

        /* Remove dead shapes from the shape tree, but don't finalize them yet. */
        {
            gcstats::AutoPhase ap(rt->gc.stats, gcstats::PHASE_SWEEP_SHAPE);

            for (; rt->gc.sweepZone; rt->gc.sweepZone = rt->gc.sweepZone->nextNodeInGroup()) {
                Zone *zone = rt->gc.sweepZone;
                while (ArenaHeader *arena = zone->allocator.arenas.gcShapeArenasToSweep) {
                    for (ArenaCellIterUnderGC i(arena); !i.done(); i.next()) {
                        Shape *shape = i.get<Shape>();
                        if (!shape->isMarked())
                            shape->sweep();
                    }

                    zone->allocator.arenas.gcShapeArenasToSweep = arena->next;
                    sliceBudget.step(Arena::thingsPerArena(Arena::thingSize(FINALIZE_SHAPE)));
                    if (sliceBudget.isOverBudget())
                        return false;  /* Yield to the mutator. */
                }
            }
        }

        EndSweepingZoneGroup(rt);
        GetNextZoneGroup(rt);
        if (!rt->gc.currentZoneGroup)
            return true;  /* We're finished. */
        EndMarkingZoneGroup(rt);
        BeginSweepingZoneGroup(rt);
    }
}

static void
EndSweepPhase(JSRuntime *rt, JSGCInvocationKind gckind, bool lastGC)
{
    gcstats::AutoPhase ap(rt->gc.stats, gcstats::PHASE_SWEEP);
    FreeOp fop(rt, rt->gc.sweepOnBackgroundThread);

    JS_ASSERT_IF(lastGC, !rt->gc.sweepOnBackgroundThread);

    JS_ASSERT(rt->gc.marker.isDrained());
    rt->gc.marker.stop();

    /*
     * Recalculate whether GC was full or not as this may have changed due to
     * newly created zones.  Can only change from full to not full.
     */
    if (rt->gc.isFull) {
        for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
            if (!zone->isCollecting()) {
                rt->gc.isFull = false;
                break;
            }
        }
    }

    /*
     * If we found any black->gray edges during marking, we completely clear the
     * mark bits of all uncollected zones, or if a reset has occured, zones that
     * will no longer be collected. This is safe, although it may
     * prevent the cycle collector from collecting some dead objects.
     */
    if (rt->gc.foundBlackGrayEdges) {
        for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
            if (!zone->isCollecting())
                zone->allocator.arenas.unmarkAll();
        }
    }

    {
        gcstats::AutoPhase ap(rt->gc.stats, gcstats::PHASE_DESTROY);

        /*
         * Sweep script filenames after sweeping functions in the generic loop
         * above. In this way when a scripted function's finalizer destroys the
         * script and calls rt->destroyScriptHook, the hook can still access the
         * script's filename. See bug 323267.
         */
        if (rt->gc.isFull)
            SweepScriptData(rt);

        /* Clear out any small pools that we're hanging on to. */
        if (JSC::ExecutableAllocator *execAlloc = rt->maybeExecAlloc())
            execAlloc->purge();
#ifdef JS_ION
        if (rt->jitRuntime() && rt->jitRuntime()->hasIonAlloc()) {
            JSRuntime::AutoLockForInterrupt lock(rt);
            rt->jitRuntime()->ionAlloc(rt)->purge();
        }
#endif

        /*
         * This removes compartments from rt->compartment, so we do it last to make
         * sure we don't miss sweeping any compartments.
         */
        if (!lastGC)
            SweepZones(&fop, lastGC);

        if (!rt->gc.sweepOnBackgroundThread) {
            /*
             * Destroy arenas after we finished the sweeping so finalizers can
             * safely use IsAboutToBeFinalized(). This is done on the
             * GCHelperThread if possible. We acquire the lock only because
             * Expire needs to unlock it for other callers.
             */
            AutoLockGC lock(rt);
            ExpireChunksAndArenas(rt, gckind == GC_SHRINK);
        }
    }

    {
        gcstats::AutoPhase ap(rt->gc.stats, gcstats::PHASE_FINALIZE_END);

        if (rt->gc.finalizeCallback)
            rt->gc.finalizeCallback(&fop, JSFINALIZE_COLLECTION_END, !rt->gc.isFull);

        /* If we finished a full GC, then the gray bits are correct. */
        if (rt->gc.isFull)
            rt->gc.grayBitsValid = true;
    }

    /* Set up list of zones for sweeping of background things. */
    JS_ASSERT(!rt->gc.sweepingZones);
    for (GCZonesIter zone(rt); !zone.done(); zone.next()) {
        zone->gcNextGraphNode = rt->gc.sweepingZones;
        rt->gc.sweepingZones = zone;
    }

    /* If not sweeping on background thread then we must do it here. */
    if (!rt->gc.sweepOnBackgroundThread) {
        gcstats::AutoPhase ap(rt->gc.stats, gcstats::PHASE_DESTROY);

        SweepBackgroundThings(rt, false);

        rt->freeLifoAlloc.freeAll();

        /* Ensure the compartments get swept if it's the last GC. */
        if (lastGC)
            SweepZones(&fop, lastGC);
    }

    for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
        zone->setGCLastBytes(zone->gcBytes, gckind);
        if (zone->isCollecting()) {
            JS_ASSERT(zone->isGCFinished());
            zone->setGCState(Zone::NoGC);
        }

#ifdef DEBUG
        JS_ASSERT(!zone->isCollecting());
        JS_ASSERT(!zone->wasGCStarted());

        for (unsigned i = 0 ; i < FINALIZE_LIMIT ; ++i) {
            JS_ASSERT_IF(!IsBackgroundFinalized(AllocKind(i)) ||
                         !rt->gc.sweepOnBackgroundThread,
                         !zone->allocator.arenas.arenaListsToSweep[i]);
        }
#endif
    }

#ifdef DEBUG
    for (CompartmentsIter c(rt, SkipAtoms); !c.done(); c.next()) {
        JS_ASSERT(!c->gcIncomingGrayPointers);
        JS_ASSERT(c->gcLiveArrayBuffers.empty());

        for (JSCompartment::WrapperEnum e(c); !e.empty(); e.popFront()) {
            if (e.front().key().kind != CrossCompartmentKey::StringWrapper)
                AssertNotOnGrayList(&e.front().value().get().toObject());
        }
    }
#endif

    FinishMarkingValidation(rt);

    rt->gc.lastGCTime = PRMJ_Now();
}

namespace {

/* ...while this class is to be used only for garbage collection. */
class AutoGCSession
{
    JSRuntime *runtime;
    AutoTraceSession session;
    bool canceled;

  public:
    explicit AutoGCSession(JSRuntime *rt);
    ~AutoGCSession();

    void cancel() { canceled = true; }
};

} /* anonymous namespace */

/* Start a new heap session. */
AutoTraceSession::AutoTraceSession(JSRuntime *rt, js::HeapState heapState)
  : lock(rt),
    runtime(rt),
    prevState(rt->gc.heapState)
{
    JS_ASSERT(!rt->gc.noGCOrAllocationCheck);
    JS_ASSERT(!rt->isHeapBusy());
    JS_ASSERT(heapState != Idle);
#ifdef JSGC_GENERATIONAL
    JS_ASSERT_IF(heapState == MajorCollecting, rt->gc.nursery.isEmpty());
#endif

    // Threads with an exclusive context can hit refillFreeList while holding
    // the exclusive access lock. To avoid deadlocking when we try to acquire
    // this lock during GC and the other thread is waiting, make sure we hold
    // the exclusive access lock during GC sessions.
    JS_ASSERT(rt->currentThreadHasExclusiveAccess());

    if (rt->exclusiveThreadsPresent()) {
        // Lock the worker thread state when changing the heap state in the
        // presence of exclusive threads, to avoid racing with refillFreeList.
#ifdef JS_THREADSAFE
        AutoLockWorkerThreadState lock;
        rt->gc.heapState = heapState;
#else
        MOZ_CRASH();
#endif
    } else {
        rt->gc.heapState = heapState;
    }
}

AutoTraceSession::~AutoTraceSession()
{
    JS_ASSERT(runtime->isHeapBusy());

    if (runtime->exclusiveThreadsPresent()) {
#ifdef JS_THREADSAFE
        AutoLockWorkerThreadState lock;
        runtime->gc.heapState = prevState;

        // Notify any worker threads waiting for the trace session to end.
        WorkerThreadState().notifyAll(GlobalWorkerThreadState::PRODUCER);
#else
        MOZ_CRASH();
#endif
    } else {
        runtime->gc.heapState = prevState;
    }
}

AutoGCSession::AutoGCSession(JSRuntime *rt)
  : runtime(rt),
    session(rt, MajorCollecting),
    canceled(false)
{
    runtime->gc.isNeeded = false;
    runtime->gc.interFrameGC = true;

    runtime->gc.number++;

    // It's ok if threads other than the main thread have suppressGC set, as
    // they are operating on zones which will not be collected from here.
    JS_ASSERT(!runtime->mainThread.suppressGC);
}

AutoGCSession::~AutoGCSession()
{
    if (canceled)
        return;

#ifndef JS_MORE_DETERMINISTIC
    runtime->gc.nextFullGCTime = PRMJ_Now() + GC_IDLE_FULL_SPAN;
#endif

    runtime->gc.chunkAllocationSinceLastGC = false;

#ifdef JS_GC_ZEAL
    /* Keeping these around after a GC is dangerous. */
    runtime->gc.selectedForMarking.clearAndFree();
#endif

    /* Clear gcMallocBytes for all compartments */
    for (ZonesIter zone(runtime, WithAtoms); !zone.done(); zone.next()) {
        zone->resetGCMallocBytes();
        zone->unscheduleGC();
    }

    runtime->resetGCMallocBytes();
}

AutoCopyFreeListToArenas::AutoCopyFreeListToArenas(JSRuntime *rt, ZoneSelector selector)
  : runtime(rt),
    selector(selector)
{
    for (ZonesIter zone(rt, selector); !zone.done(); zone.next())
        zone->allocator.arenas.copyFreeListsToArenas();
}

AutoCopyFreeListToArenas::~AutoCopyFreeListToArenas()
{
    for (ZonesIter zone(runtime, selector); !zone.done(); zone.next())
        zone->allocator.arenas.clearFreeListsInArenas();
}

class AutoCopyFreeListToArenasForGC
{
    JSRuntime *runtime;

  public:
    AutoCopyFreeListToArenasForGC(JSRuntime *rt) : runtime(rt) {
        JS_ASSERT(rt->currentThreadHasExclusiveAccess());
        for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next())
            zone->allocator.arenas.copyFreeListsToArenas();
    }
    ~AutoCopyFreeListToArenasForGC() {
        for (ZonesIter zone(runtime, WithAtoms); !zone.done(); zone.next())
            zone->allocator.arenas.clearFreeListsInArenas();
    }
};

static void
IncrementalCollectSlice(JSRuntime *rt,
                        int64_t budget,
                        JS::gcreason::Reason gcReason,
                        JSGCInvocationKind gcKind);

static void
ResetIncrementalGC(JSRuntime *rt, const char *reason)
{
    switch (rt->gc.incrementalState) {
      case NO_INCREMENTAL:
        return;

      case MARK: {
        /* Cancel any ongoing marking. */
        AutoCopyFreeListToArenasForGC copy(rt);

        rt->gc.marker.reset();
        rt->gc.marker.stop();

        for (GCCompartmentsIter c(rt); !c.done(); c.next()) {
            ArrayBufferObject::resetArrayBufferList(c);
            ResetGrayList(c);
        }

        for (GCZonesIter zone(rt); !zone.done(); zone.next()) {
            JS_ASSERT(zone->isGCMarking());
            zone->setNeedsBarrier(false, Zone::UpdateIon);
            zone->setGCState(Zone::NoGC);
        }
        rt->setNeedsBarrier(false);
        AssertNeedsBarrierFlagsConsistent(rt);

        rt->gc.incrementalState = NO_INCREMENTAL;

        JS_ASSERT(!rt->gc.strictCompartmentChecking);

        break;
      }

      case SWEEP:
        rt->gc.marker.reset();

        for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next())
            zone->scheduledForDestruction = false;

        /* Finish sweeping the current zone group, then abort. */
        rt->gc.abortSweepAfterCurrentGroup = true;
        IncrementalCollectSlice(rt, SliceBudget::Unlimited, JS::gcreason::RESET, GC_NORMAL);

        {
            gcstats::AutoPhase ap(rt->gc.stats, gcstats::PHASE_WAIT_BACKGROUND_THREAD);
            rt->gc.helperThread.waitBackgroundSweepOrAllocEnd();
        }
        break;

      default:
        MOZ_ASSUME_UNREACHABLE("Invalid incremental GC state");
    }

    rt->gc.stats.reset(reason);

#ifdef DEBUG
    for (CompartmentsIter c(rt, SkipAtoms); !c.done(); c.next())
        JS_ASSERT(c->gcLiveArrayBuffers.empty());

    for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
        JS_ASSERT(!zone->needsBarrier());
        for (unsigned i = 0; i < FINALIZE_LIMIT; ++i)
            JS_ASSERT(!zone->allocator.arenas.arenaListsToSweep[i]);
    }
#endif
}

namespace {

class AutoGCSlice {
  public:
    AutoGCSlice(JSRuntime *rt);
    ~AutoGCSlice();

  private:
    JSRuntime *runtime;
};

} /* anonymous namespace */

AutoGCSlice::AutoGCSlice(JSRuntime *rt)
  : runtime(rt)
{
    /*
     * During incremental GC, the compartment's active flag determines whether
     * there are stack frames active for any of its scripts. Normally this flag
     * is set at the beginning of the mark phase. During incremental GC, we also
     * set it at the start of every phase.
     */
    for (ActivationIterator iter(rt); !iter.done(); ++iter)
        iter->compartment()->zone()->active = true;

    for (GCZonesIter zone(rt); !zone.done(); zone.next()) {
        /*
         * Clear needsBarrier early so we don't do any write barriers during
         * GC. We don't need to update the Ion barriers (which is expensive)
         * because Ion code doesn't run during GC. If need be, we'll update the
         * Ion barriers in ~AutoGCSlice.
         */
        if (zone->isGCMarking()) {
            JS_ASSERT(zone->needsBarrier());
            zone->setNeedsBarrier(false, Zone::DontUpdateIon);
        } else {
            JS_ASSERT(!zone->needsBarrier());
        }
    }
    rt->setNeedsBarrier(false);
    AssertNeedsBarrierFlagsConsistent(rt);
}

AutoGCSlice::~AutoGCSlice()
{
    /* We can't use GCZonesIter if this is the end of the last slice. */
    bool haveBarriers = false;
    for (ZonesIter zone(runtime, WithAtoms); !zone.done(); zone.next()) {
        if (zone->isGCMarking()) {
            zone->setNeedsBarrier(true, Zone::UpdateIon);
            zone->allocator.arenas.prepareForIncrementalGC(runtime);
            haveBarriers = true;
        } else {
            zone->setNeedsBarrier(false, Zone::UpdateIon);
        }
    }
    runtime->setNeedsBarrier(haveBarriers);
    AssertNeedsBarrierFlagsConsistent(runtime);
}

static void
PushZealSelectedObjects(JSRuntime *rt)
{
#ifdef JS_GC_ZEAL
    /* Push selected objects onto the mark stack and clear the list. */
    for (JSObject **obj = rt->gc.selectedForMarking.begin();
         obj != rt->gc.selectedForMarking.end(); obj++)
    {
        MarkObjectUnbarriered(&rt->gc.marker, obj, "selected obj");
    }
#endif
}

static void
IncrementalCollectSlice(JSRuntime *rt,
                        int64_t budget,
                        JS::gcreason::Reason reason,
                        JSGCInvocationKind gckind)
{
    JS_ASSERT(rt->currentThreadHasExclusiveAccess());

    AutoCopyFreeListToArenasForGC copy(rt);
    AutoGCSlice slice(rt);

    bool lastGC = (reason == JS::gcreason::DESTROY_RUNTIME);

    gc::State initialState = rt->gc.incrementalState;

    int zeal = 0;
#ifdef JS_GC_ZEAL
    if (reason == JS::gcreason::DEBUG_GC && budget != SliceBudget::Unlimited) {
        /*
         * Do the incremental collection type specified by zeal mode if the
         * collection was triggered by RunDebugGC() and incremental GC has not
         * been cancelled by ResetIncrementalGC.
         */
        zeal = rt->gcZeal();
    }
#endif

    JS_ASSERT_IF(rt->gc.incrementalState != NO_INCREMENTAL, rt->gc.isIncremental);
    rt->gc.isIncremental = budget != SliceBudget::Unlimited;

    if (zeal == ZealIncrementalRootsThenFinish || zeal == ZealIncrementalMarkAllThenFinish) {
        /*
         * Yields between slices occurs at predetermined points in these modes;
         * the budget is not used.
         */
        budget = SliceBudget::Unlimited;
    }

    SliceBudget sliceBudget(budget);

    if (rt->gc.incrementalState == NO_INCREMENTAL) {
        rt->gc.incrementalState = MARK_ROOTS;
        rt->gc.lastMarkSlice = false;
    }

    if (rt->gc.incrementalState == MARK)
        AutoGCRooter::traceAllWrappers(&rt->gc.marker);

    switch (rt->gc.incrementalState) {

      case MARK_ROOTS:
        if (!BeginMarkPhase(rt)) {
            rt->gc.incrementalState = NO_INCREMENTAL;
            return;
        }

        if (!lastGC)
            PushZealSelectedObjects(rt);

        rt->gc.incrementalState = MARK;

        if (rt->gc.isIncremental && zeal == ZealIncrementalRootsThenFinish)
            break;

        /* fall through */

      case MARK: {
        /* If we needed delayed marking for gray roots, then collect until done. */
        if (!rt->gc.marker.hasBufferedGrayRoots()) {
            sliceBudget.reset();
            rt->gc.isIncremental = false;
        }

        bool finished = DrainMarkStack(rt, sliceBudget, gcstats::PHASE_MARK);
        if (!finished)
            break;

        JS_ASSERT(rt->gc.marker.isDrained());

        if (!rt->gc.lastMarkSlice && rt->gc.isIncremental &&
            ((initialState == MARK && zeal != ZealIncrementalRootsThenFinish) ||
             zeal == ZealIncrementalMarkAllThenFinish))
        {
            /*
             * Yield with the aim of starting the sweep in the next
             * slice.  We will need to mark anything new on the stack
             * when we resume, so we stay in MARK state.
             */
            rt->gc.lastMarkSlice = true;
            break;
        }

        rt->gc.incrementalState = SWEEP;

        /*
         * This runs to completion, but we don't continue if the budget is
         * now exhasted.
         */
        BeginSweepPhase(rt, lastGC);
        if (sliceBudget.isOverBudget())
            break;

        /*
         * Always yield here when running in incremental multi-slice zeal
         * mode, so RunDebugGC can reset the slice buget.
         */
        if (rt->gc.isIncremental && zeal == ZealIncrementalMultipleSlices)
            break;

        /* fall through */
      }

      case SWEEP: {
        bool finished = SweepPhase(rt, sliceBudget);
        if (!finished)
            break;

        EndSweepPhase(rt, gckind, lastGC);

        if (rt->gc.sweepOnBackgroundThread)
            rt->gc.helperThread.startBackgroundSweep(gckind == GC_SHRINK);

        rt->gc.incrementalState = NO_INCREMENTAL;
        break;
      }

      default:
        JS_ASSERT(false);
    }
}

IncrementalSafety
gc::IsIncrementalGCSafe(JSRuntime *rt)
{
    JS_ASSERT(!rt->mainThread.suppressGC);

    if (rt->keepAtoms())
        return IncrementalSafety::Unsafe("keepAtoms set");

    if (!rt->gc.incrementalEnabled)
        return IncrementalSafety::Unsafe("incremental permanently disabled");

    return IncrementalSafety::Safe();
}

static void
BudgetIncrementalGC(JSRuntime *rt, int64_t *budget)
{
    IncrementalSafety safe = IsIncrementalGCSafe(rt);
    if (!safe) {
        ResetIncrementalGC(rt, safe.reason());
        *budget = SliceBudget::Unlimited;
        rt->gc.stats.nonincremental(safe.reason());
        return;
    }

    if (rt->gcMode() != JSGC_MODE_INCREMENTAL) {
        ResetIncrementalGC(rt, "GC mode change");
        *budget = SliceBudget::Unlimited;
        rt->gc.stats.nonincremental("GC mode");
        return;
    }

    if (rt->isTooMuchMalloc()) {
        *budget = SliceBudget::Unlimited;
        rt->gc.stats.nonincremental("malloc bytes trigger");
    }

    bool reset = false;
    for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
        if (zone->gcBytes >= zone->gcTriggerBytes) {
            *budget = SliceBudget::Unlimited;
            rt->gc.stats.nonincremental("allocation trigger");
        }

        if (rt->gc.incrementalState != NO_INCREMENTAL &&
            zone->isGCScheduled() != zone->wasGCStarted())
        {
            reset = true;
        }

        if (zone->isTooMuchMalloc()) {
            *budget = SliceBudget::Unlimited;
            rt->gc.stats.nonincremental("malloc bytes trigger");
        }
    }

    if (reset)
        ResetIncrementalGC(rt, "zone change");
}

/*
 * Run one GC "cycle" (either a slice of incremental GC or an entire
 * non-incremental GC. We disable inlining to ensure that the bottom of the
 * stack with possible GC roots recorded in MarkRuntime excludes any pointers we
 * use during the marking implementation.
 *
 * Returns true if we "reset" an existing incremental GC, which would force us
 * to run another cycle.
 */
static MOZ_NEVER_INLINE bool
GCCycle(JSRuntime *rt, bool incremental, int64_t budget,
        JSGCInvocationKind gckind, JS::gcreason::Reason reason)
{
    AutoGCSession gcsession(rt);

    /*
     * As we about to purge caches and clear the mark bits we must wait for
     * any background finalization to finish. We must also wait for the
     * background allocation to finish so we can avoid taking the GC lock
     * when manipulating the chunks during the GC.
     */
    {
        gcstats::AutoPhase ap(rt->gc.stats, gcstats::PHASE_WAIT_BACKGROUND_THREAD);
        rt->gc.helperThread.waitBackgroundSweepOrAllocEnd();
    }

    State prevState = rt->gc.incrementalState;

    if (!incremental) {
        /* If non-incremental GC was requested, reset incremental GC. */
        ResetIncrementalGC(rt, "requested");
        rt->gc.stats.nonincremental("requested");
        budget = SliceBudget::Unlimited;
    } else {
        BudgetIncrementalGC(rt, &budget);
    }

    /* The GC was reset, so we need a do-over. */
    if (prevState != NO_INCREMENTAL && rt->gc.incrementalState == NO_INCREMENTAL) {
        gcsession.cancel();
        return true;
    }

    IncrementalCollectSlice(rt, budget, reason, gckind);
    return false;
}

#ifdef JS_GC_ZEAL
static bool
IsDeterministicGCReason(JS::gcreason::Reason reason)
{
    if (reason > JS::gcreason::DEBUG_GC &&
        reason != JS::gcreason::CC_FORCED && reason != JS::gcreason::SHUTDOWN_CC)
    {
        return false;
    }

    if (reason == JS::gcreason::MAYBEGC)
        return false;

    return true;
}
#endif

static bool
ShouldCleanUpEverything(JSRuntime *rt, JS::gcreason::Reason reason, JSGCInvocationKind gckind)
{
    // During shutdown, we must clean everything up, for the sake of leak
    // detection. When a runtime has no contexts, or we're doing a GC before a
    // shutdown CC, those are strong indications that we're shutting down.
    return reason == JS::gcreason::DESTROY_RUNTIME ||
           reason == JS::gcreason::SHUTDOWN_CC ||
           gckind == GC_SHRINK;
}

namespace {

#ifdef JSGC_GENERATIONAL
class AutoDisableStoreBuffer
{
    JSRuntime *runtime;
    bool prior;

  public:
    AutoDisableStoreBuffer(JSRuntime *rt) : runtime(rt) {
        prior = rt->gc.storeBuffer.isEnabled();
        rt->gc.storeBuffer.disable();
    }
    ~AutoDisableStoreBuffer() {
        if (prior)
            runtime->gc.storeBuffer.enable();
    }
};
#else
struct AutoDisableStoreBuffer
{
    AutoDisableStoreBuffer(JSRuntime *) {}
};
#endif

} /* anonymous namespace */

static void
Collect(JSRuntime *rt, bool incremental, int64_t budget,
        JSGCInvocationKind gckind, JS::gcreason::Reason reason)
{
    /* GC shouldn't be running in parallel execution mode */
    JS_ASSERT(!InParallelSection());

    JS_AbortIfWrongThread(rt);

    /* If we attempt to invoke the GC while we are running in the GC, assert. */
    JS_ASSERT(!rt->isHeapBusy());

    if (rt->mainThread.suppressGC)
        return;

    TraceLogger *logger = TraceLoggerForMainThread(rt);
    AutoTraceLog logGC(logger, TraceLogger::GC);

#ifdef JS_GC_ZEAL
    if (rt->gc.deterministicOnly && !IsDeterministicGCReason(reason))
        return;
#endif

    JS_ASSERT_IF(!incremental || budget != SliceBudget::Unlimited, JSGC_INCREMENTAL);

    AutoStopVerifyingBarriers av(rt, reason == JS::gcreason::SHUTDOWN_CC ||
                                     reason == JS::gcreason::DESTROY_RUNTIME);

    RecordNativeStackTopForGC(rt);

    int zoneCount = 0;
    int compartmentCount = 0;
    int collectedCount = 0;
    for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
        if (rt->gcMode() == JSGC_MODE_GLOBAL)
            zone->scheduleGC();

        /* This is a heuristic to avoid resets. */
        if (rt->gc.incrementalState != NO_INCREMENTAL && zone->needsBarrier())
            zone->scheduleGC();

        zoneCount++;
        if (zone->isGCScheduled())
            collectedCount++;
    }

    for (CompartmentsIter c(rt, WithAtoms); !c.done(); c.next())
        compartmentCount++;

    rt->gc.shouldCleanUpEverything = ShouldCleanUpEverything(rt, reason, gckind);

    bool repeat = false;
    do {
        MinorGC(rt, reason);

        /*
         * Marking can trigger many incidental post barriers, some of them for
         * objects which are not going to be live after the GC.
         */
        AutoDisableStoreBuffer adsb(rt);

        gcstats::AutoGCSlice agc(rt->gc.stats, collectedCount, zoneCount, compartmentCount, reason);

        /*
         * Let the API user decide to defer a GC if it wants to (unless this
         * is the last context). Invoke the callback regardless.
         */
        if (rt->gc.incrementalState == NO_INCREMENTAL) {
            gcstats::AutoPhase ap(rt->gc.stats, gcstats::PHASE_GC_BEGIN);
            if (JSGCCallback callback = rt->gc.callback)
                callback(rt, JSGC_BEGIN, rt->gc.callbackData);
        }

        rt->gc.poke = false;
        bool wasReset = GCCycle(rt, incremental, budget, gckind, reason);

        if (rt->gc.incrementalState == NO_INCREMENTAL) {
            gcstats::AutoPhase ap(rt->gc.stats, gcstats::PHASE_GC_END);
            if (JSGCCallback callback = rt->gc.callback)
                callback(rt, JSGC_END, rt->gc.callbackData);
        }

        /* Need to re-schedule all zones for GC. */
        if (rt->gc.poke && rt->gc.shouldCleanUpEverything)
            JS::PrepareForFullGC(rt);

        /*
         * If we reset an existing GC, we need to start a new one. Also, we
         * repeat GCs that happen during shutdown (the gcShouldCleanUpEverything
         * case) until we can be sure that no additional garbage is created
         * (which typically happens if roots are dropped during finalizers).
         */
        repeat = (rt->gc.poke && rt->gc.shouldCleanUpEverything) || wasReset;
    } while (repeat);

    if (rt->gc.incrementalState == NO_INCREMENTAL) {
#ifdef JS_THREADSAFE
        EnqueuePendingParseTasksAfterGC(rt);
#endif
    }
}

void
js::GC(JSRuntime *rt, JSGCInvocationKind gckind, JS::gcreason::Reason reason)
{
    Collect(rt, false, SliceBudget::Unlimited, gckind, reason);
}

void
js::GCSlice(JSRuntime *rt, JSGCInvocationKind gckind, JS::gcreason::Reason reason, int64_t millis)
{
    int64_t sliceBudget;
    if (millis)
        sliceBudget = SliceBudget::TimeBudget(millis);
    else if (rt->gc.highFrequencyGC && rt->gc.dynamicMarkSlice)
        sliceBudget = rt->gc.sliceBudget * IGC_MARK_SLICE_MULTIPLIER;
    else
        sliceBudget = rt->gc.sliceBudget;

    Collect(rt, true, sliceBudget, gckind, reason);
}

void
js::GCFinalSlice(JSRuntime *rt, JSGCInvocationKind gckind, JS::gcreason::Reason reason)
{
    Collect(rt, true, SliceBudget::Unlimited, gckind, reason);
}

static bool
ZonesSelected(JSRuntime *rt)
{
    for (ZonesIter zone(rt, WithAtoms); !zone.done(); zone.next()) {
        if (zone->isGCScheduled())
            return true;
    }
    return false;
}

void
js::GCDebugSlice(JSRuntime *rt, bool limit, int64_t objCount)
{
    int64_t budget = limit ? SliceBudget::WorkBudget(objCount) : SliceBudget::Unlimited;
    if (!ZonesSelected(rt)) {
        if (JS::IsIncrementalGCInProgress(rt))
            JS::PrepareForIncrementalGC(rt);
        else
            JS::PrepareForFullGC(rt);
    }
    Collect(rt, true, budget, GC_NORMAL, JS::gcreason::DEBUG_GC);
}

/* Schedule a full GC unless a zone will already be collected. */
void
js::PrepareForDebugGC(JSRuntime *rt)
{
    if (!ZonesSelected(rt))
        JS::PrepareForFullGC(rt);
}

JS_FRIEND_API(void)
JS::ShrinkGCBuffers(JSRuntime *rt)
{
    AutoLockGC lock(rt);
    JS_ASSERT(!rt->isHeapBusy());

    if (!rt->useHelperThreads())
        ExpireChunksAndArenas(rt, true);
    else
        rt->gc.helperThread.startBackgroundShrink();
}

void
js::MinorGC(JSRuntime *rt, JS::gcreason::Reason reason)
{
#ifdef JSGC_GENERATIONAL
    TraceLogger *logger = TraceLoggerForMainThread(rt);
    AutoTraceLog logMinorGC(logger, TraceLogger::MinorGC);
    rt->gc.nursery.collect(rt, reason, nullptr);
    JS_ASSERT_IF(!rt->mainThread.suppressGC, rt->gc.nursery.isEmpty());
#endif
}

void
js::MinorGC(JSContext *cx, JS::gcreason::Reason reason)
{
    // Alternate to the runtime-taking form above which allows marking type
    // objects as needing pretenuring.
#ifdef JSGC_GENERATIONAL
    TraceLogger *logger = TraceLoggerForMainThread(cx->runtime());
    AutoTraceLog logMinorGC(logger, TraceLogger::MinorGC);

    Nursery::TypeObjectList pretenureTypes;
    JSRuntime *rt = cx->runtime();
    rt->gc.nursery.collect(cx->runtime(), reason, &pretenureTypes);
    for (size_t i = 0; i < pretenureTypes.length(); i++) {
        if (pretenureTypes[i]->canPreTenure())
            pretenureTypes[i]->setShouldPreTenure(cx);
    }
    JS_ASSERT_IF(!rt->mainThread.suppressGC, rt->gc.nursery.isEmpty());
#endif
}

void
js::gc::GCIfNeeded(JSContext *cx)
{
    JSRuntime *rt = cx->runtime();

#ifdef JSGC_GENERATIONAL
    /*
     * In case of store buffer overflow perform minor GC first so that the
     * correct reason is seen in the logs.
     */
    if (rt->gc.storeBuffer.isAboutToOverflow())
        MinorGC(cx, JS::gcreason::FULL_STORE_BUFFER);
#endif

    if (rt->gc.isNeeded)
        GCSlice(rt, GC_NORMAL, rt->gc.triggerReason);
}

void
js::gc::FinishBackgroundFinalize(JSRuntime *rt)
{
    rt->gc.helperThread.waitBackgroundSweepEnd();
}

AutoFinishGC::AutoFinishGC(JSRuntime *rt)
{
    if (JS::IsIncrementalGCInProgress(rt)) {
        JS::PrepareForIncrementalGC(rt);
        JS::FinishIncrementalGC(rt, JS::gcreason::API);
    }

    gc::FinishBackgroundFinalize(rt);
}

AutoPrepareForTracing::AutoPrepareForTracing(JSRuntime *rt, ZoneSelector selector)
  : finish(rt),
    session(rt),
    copy(rt, selector)
{
    RecordNativeStackTopForGC(rt);
}

JSCompartment *
js::NewCompartment(JSContext *cx, Zone *zone, JSPrincipals *principals,
                   const JS::CompartmentOptions &options)
{
    JSRuntime *rt = cx->runtime();
    JS_AbortIfWrongThread(rt);

    ScopedJSDeletePtr<Zone> zoneHolder;
    if (!zone) {
        zone = cx->new_<Zone>(rt);
        if (!zone)
            return nullptr;

        zoneHolder.reset(zone);

        zone->setGCLastBytes(8192, GC_NORMAL);

        const JSPrincipals *trusted = rt->trustedPrincipals();
        zone->isSystem = principals && principals == trusted;
    }

    ScopedJSDeletePtr<JSCompartment> compartment(cx->new_<JSCompartment>(zone, options));
    if (!compartment || !compartment->init(cx))
        return nullptr;

    // Set up the principals.
    JS_SetCompartmentPrincipals(compartment, principals);

    AutoLockGC lock(rt);

    if (!zone->compartments.append(compartment.get())) {
        js_ReportOutOfMemory(cx);
        return nullptr;
    }

    if (zoneHolder && !rt->gc.zones.append(zone)) {
        js_ReportOutOfMemory(cx);
        return nullptr;
    }

    zoneHolder.forget();
    return compartment.forget();
}

void
gc::MergeCompartments(JSCompartment *source, JSCompartment *target)
{
    // The source compartment must be specifically flagged as mergable.  This
    // also implies that the compartment is not visible to the debugger.
    JS_ASSERT(source->options_.mergeable());

    JSRuntime *rt = source->runtimeFromMainThread();

    AutoPrepareForTracing prepare(rt, SkipAtoms);

    // Cleanup tables and other state in the source compartment that will be
    // meaningless after merging into the target compartment.

    source->clearTables();

    // Fixup compartment pointers in source to refer to target.

    for (ZoneCellIter iter(source->zone(), FINALIZE_SCRIPT); !iter.done(); iter.next()) {
        JSScript *script = iter.get<JSScript>();
        JS_ASSERT(script->compartment() == source);
        script->compartment_ = target;
    }

    for (ZoneCellIter iter(source->zone(), FINALIZE_BASE_SHAPE); !iter.done(); iter.next()) {
        BaseShape *base = iter.get<BaseShape>();
        JS_ASSERT(base->compartment() == source);
        base->compartment_ = target;
    }

    // Fixup zone pointers in source's zone to refer to target's zone.

    for (size_t thingKind = 0; thingKind != FINALIZE_LIMIT; thingKind++) {
        for (ArenaIter aiter(source->zone(), AllocKind(thingKind)); !aiter.done(); aiter.next()) {
            ArenaHeader *aheader = aiter.get();
            aheader->zone = target->zone();
        }
    }

    // The source should be the only compartment in its zone.
    for (CompartmentsInZoneIter c(source->zone()); !c.done(); c.next())
        JS_ASSERT(c.get() == source);

    // Merge the allocator in source's zone into target's zone.
    target->zone()->allocator.arenas.adoptArenas(rt, &source->zone()->allocator.arenas);
    target->zone()->gcBytes += source->zone()->gcBytes;
    source->zone()->gcBytes = 0;

    // Merge other info in source's zone into target's zone.
    target->zone()->types.typeLifoAlloc.transferFrom(&source->zone()->types.typeLifoAlloc);
}

void
gc::RunDebugGC(JSContext *cx)
{
#ifdef JS_GC_ZEAL
    JSRuntime *rt = cx->runtime();
    int type = rt->gcZeal();

    if (rt->mainThread.suppressGC)
        return;

    if (type == js::gc::ZealGenerationalGCValue)
        return MinorGC(rt, JS::gcreason::DEBUG_GC);

    PrepareForDebugGC(cx->runtime());

    if (type == ZealIncrementalRootsThenFinish ||
        type == ZealIncrementalMarkAllThenFinish ||
        type == ZealIncrementalMultipleSlices)
    {
        js::gc::State initialState = rt->gc.incrementalState;
        int64_t budget;
        if (type == ZealIncrementalMultipleSlices) {
            /*
             * Start with a small slice limit and double it every slice. This
             * ensure that we get multiple slices, and collection runs to
             * completion.
             */
            if (initialState == NO_INCREMENTAL)
                rt->gc.incrementalLimit = rt->gc.zealFrequency / 2;
            else
                rt->gc.incrementalLimit *= 2;
            budget = SliceBudget::WorkBudget(rt->gc.incrementalLimit);
        } else {
            // This triggers incremental GC but is actually ignored by IncrementalMarkSlice.
            budget = SliceBudget::WorkBudget(1);
        }

        Collect(rt, true, budget, GC_NORMAL, JS::gcreason::DEBUG_GC);

        /*
         * For multi-slice zeal, reset the slice size when we get to the sweep
         * phase.
         */
        if (type == ZealIncrementalMultipleSlices &&
            initialState == MARK && rt->gc.incrementalState == SWEEP)
        {
            rt->gc.incrementalLimit = rt->gc.zealFrequency / 2;
        }
    } else {
        Collect(rt, false, SliceBudget::Unlimited, GC_NORMAL, JS::gcreason::DEBUG_GC);
    }

#endif
}

void
gc::SetDeterministicGC(JSContext *cx, bool enabled)
{
#ifdef JS_GC_ZEAL
    JSRuntime *rt = cx->runtime();
    rt->gc.deterministicOnly = enabled;
#endif
}

void
gc::SetValidateGC(JSContext *cx, bool enabled)
{
    JSRuntime *rt = cx->runtime();
    rt->gc.validate = enabled;
}

void
gc::SetFullCompartmentChecks(JSContext *cx, bool enabled)
{
    JSRuntime *rt = cx->runtime();
    rt->gc.fullCompartmentChecks = enabled;
}

#ifdef DEBUG

/* Should only be called manually under gdb */
void PreventGCDuringInteractiveDebug()
{
    TlsPerThreadData.get()->suppressGC++;
}

#endif

void
js::ReleaseAllJITCode(FreeOp *fop)
{
#ifdef JS_ION

# ifdef JSGC_GENERATIONAL
    /*
     * Scripts can entrain nursery things, inserting references to the script
     * into the store buffer. Clear the store buffer before discarding scripts.
     */
    MinorGC(fop->runtime(), JS::gcreason::EVICT_NURSERY);
# endif

    for (ZonesIter zone(fop->runtime(), SkipAtoms); !zone.done(); zone.next()) {
        if (!zone->jitZone())
            continue;

# ifdef DEBUG
        /* Assert no baseline scripts are marked as active. */
        for (ZoneCellIter i(zone, FINALIZE_SCRIPT); !i.done(); i.next()) {
            JSScript *script = i.get<JSScript>();
            JS_ASSERT_IF(script->hasBaselineScript(), !script->baselineScript()->active());
        }
# endif

        /* Mark baseline scripts on the stack as active. */
        jit::MarkActiveBaselineScripts(zone);

        jit::InvalidateAll(fop, zone);

        for (ZoneCellIter i(zone, FINALIZE_SCRIPT); !i.done(); i.next()) {
            JSScript *script = i.get<JSScript>();
            jit::FinishInvalidation<SequentialExecution>(fop, script);
            jit::FinishInvalidation<ParallelExecution>(fop, script);

            /*
             * Discard baseline script if it's not marked as active. Note that
             * this also resets the active flag.
             */
            jit::FinishDiscardBaselineScript(fop, script);
        }

        zone->jitZone()->optimizedStubSpace()->free();
    }
#endif
}

/*
 * There are three possible PCCount profiling states:
 *
 * 1. None: Neither scripts nor the runtime have count information.
 * 2. Profile: Active scripts have count information, the runtime does not.
 * 3. Query: Scripts do not have count information, the runtime does.
 *
 * When starting to profile scripts, counting begins immediately, with all JIT
 * code discarded and recompiled with counts as necessary. Active interpreter
 * frames will not begin profiling until they begin executing another script
 * (via a call or return).
 *
 * The below API functions manage transitions to new states, according
 * to the table below.
 *
 *                                  Old State
 *                          -------------------------
 * Function                 None      Profile   Query
 * --------
 * StartPCCountProfiling    Profile   Profile   Profile
 * StopPCCountProfiling     None      Query     Query
 * PurgePCCounts            None      None      None
 */

static void
ReleaseScriptCounts(FreeOp *fop)
{
    JSRuntime *rt = fop->runtime();
    JS_ASSERT(rt->gc.scriptAndCountsVector);

    ScriptAndCountsVector &vec = *rt->gc.scriptAndCountsVector;

    for (size_t i = 0; i < vec.length(); i++)
        vec[i].scriptCounts.destroy(fop);

    fop->delete_(rt->gc.scriptAndCountsVector);
    rt->gc.scriptAndCountsVector = nullptr;
}

JS_FRIEND_API(void)
js::StartPCCountProfiling(JSContext *cx)
{
    JSRuntime *rt = cx->runtime();

    if (rt->profilingScripts)
        return;

    if (rt->gc.scriptAndCountsVector)
        ReleaseScriptCounts(rt->defaultFreeOp());

    ReleaseAllJITCode(rt->defaultFreeOp());

    rt->profilingScripts = true;
}

JS_FRIEND_API(void)
js::StopPCCountProfiling(JSContext *cx)
{
    JSRuntime *rt = cx->runtime();

    if (!rt->profilingScripts)
        return;
    JS_ASSERT(!rt->gc.scriptAndCountsVector);

    ReleaseAllJITCode(rt->defaultFreeOp());

    ScriptAndCountsVector *vec = cx->new_<ScriptAndCountsVector>(SystemAllocPolicy());
    if (!vec)
        return;

    for (ZonesIter zone(rt, SkipAtoms); !zone.done(); zone.next()) {
        for (ZoneCellIter i(zone, FINALIZE_SCRIPT); !i.done(); i.next()) {
            JSScript *script = i.get<JSScript>();
            if (script->hasScriptCounts() && script->types) {
                ScriptAndCounts sac;
                sac.script = script;
                sac.scriptCounts.set(script->releaseScriptCounts());
                if (!vec->append(sac))
                    sac.scriptCounts.destroy(rt->defaultFreeOp());
            }
        }
    }

    rt->profilingScripts = false;
    rt->gc.scriptAndCountsVector = vec;
}

JS_FRIEND_API(void)
js::PurgePCCounts(JSContext *cx)
{
    JSRuntime *rt = cx->runtime();

    if (!rt->gc.scriptAndCountsVector)
        return;
    JS_ASSERT(!rt->profilingScripts);

    ReleaseScriptCounts(rt->defaultFreeOp());
}

void
js::PurgeJITCaches(Zone *zone)
{
#ifdef JS_ION
    for (ZoneCellIterUnderGC i(zone, FINALIZE_SCRIPT); !i.done(); i.next()) {
        JSScript *script = i.get<JSScript>();

        /* Discard Ion caches. */
        jit::PurgeCaches(script, zone);
    }
#endif
}

void
ArenaLists::normalizeBackgroundFinalizeState(AllocKind thingKind)
{
    volatile uintptr_t *bfs = &backgroundFinalizeState[thingKind];
    switch (*bfs) {
      case BFS_DONE:
        break;
      case BFS_JUST_FINISHED:
        // No allocations between end of last sweep and now.
        // Transfering over arenas is a kind of allocation.
        *bfs = BFS_DONE;
        break;
      default:
        JS_ASSERT(!"Background finalization in progress, but it should not be.");
        break;
    }
}

void
ArenaLists::adoptArenas(JSRuntime *rt, ArenaLists *fromArenaLists)
{
    // The other parallel threads have all completed now, and GC
    // should be inactive, but still take the lock as a kind of read
    // fence.
    AutoLockGC lock(rt);

    fromArenaLists->purge();

    for (size_t thingKind = 0; thingKind != FINALIZE_LIMIT; thingKind++) {
#ifdef JS_THREADSAFE
        // When we enter a parallel section, we join the background
        // thread, and we do not run GC while in the parallel section,
        // so no finalizer should be active!
        normalizeBackgroundFinalizeState(AllocKind(thingKind));
        fromArenaLists->normalizeBackgroundFinalizeState(AllocKind(thingKind));
#endif
        ArenaList *fromList = &fromArenaLists->arenaLists[thingKind];
        ArenaList *toList = &arenaLists[thingKind];
        while (fromList->head != nullptr) {
            // Remove entry from |fromList|
            ArenaHeader *fromHeader = fromList->head;
            fromList->head = fromHeader->next;
            fromHeader->next = nullptr;

            // During parallel execution, we sometimes keep empty arenas
            // on the lists rather than sending them back to the chunk.
            // Therefore, if fromHeader is empty, send it back to the
            // chunk now. Otherwise, attach to |toList|.
            if (fromHeader->isEmpty())
                fromHeader->chunk()->releaseArena(fromHeader);
            else
                toList->insert(fromHeader);
        }
        fromList->cursor = &fromList->head;
    }
}

bool
ArenaLists::containsArena(JSRuntime *rt, ArenaHeader *needle)
{
    AutoLockGC lock(rt);
    size_t allocKind = needle->getAllocKind();
    for (ArenaHeader *aheader = arenaLists[allocKind].head;
         aheader != nullptr;
         aheader = aheader->next)
    {
        if (aheader == needle)
            return true;
    }
    return false;
}


AutoMaybeTouchDeadZones::AutoMaybeTouchDeadZones(JSContext *cx)
  : runtime(cx->runtime()),
    markCount(runtime->gc.objectsMarkedInDeadZones),
    inIncremental(JS::IsIncrementalGCInProgress(runtime)),
    manipulatingDeadZones(runtime->gc.manipulatingDeadZones)
{
    runtime->gc.manipulatingDeadZones = true;
}

AutoMaybeTouchDeadZones::AutoMaybeTouchDeadZones(JSObject *obj)
  : runtime(obj->compartment()->runtimeFromMainThread()),
    markCount(runtime->gc.objectsMarkedInDeadZones),
    inIncremental(JS::IsIncrementalGCInProgress(runtime)),
    manipulatingDeadZones(runtime->gc.manipulatingDeadZones)
{
    runtime->gc.manipulatingDeadZones = true;
}

AutoMaybeTouchDeadZones::~AutoMaybeTouchDeadZones()
{
    runtime->gc.manipulatingDeadZones = manipulatingDeadZones;

    if (inIncremental && runtime->gc.objectsMarkedInDeadZones != markCount) {
        JS::PrepareForFullGC(runtime);
        js::GC(runtime, GC_NORMAL, JS::gcreason::TRANSPLANT);
    }
}

AutoSuppressGC::AutoSuppressGC(ExclusiveContext *cx)
  : suppressGC_(cx->perThreadData->suppressGC)
{
    suppressGC_++;
}

AutoSuppressGC::AutoSuppressGC(JSCompartment *comp)
  : suppressGC_(comp->runtimeFromMainThread()->mainThread.suppressGC)
{
    suppressGC_++;
}

AutoSuppressGC::AutoSuppressGC(JSRuntime *rt)
  : suppressGC_(rt->mainThread.suppressGC)
{
    suppressGC_++;
}

bool
js::UninlinedIsInsideNursery(JSRuntime *rt, const void *thing)
{
    return IsInsideNursery(rt, thing);
}

#ifdef DEBUG
AutoDisableProxyCheck::AutoDisableProxyCheck(JSRuntime *rt
                                             MOZ_GUARD_OBJECT_NOTIFIER_PARAM_IN_IMPL)
  : count(rt->gc.disableStrictProxyCheckingCount)
{
    MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    count++;
}

JS_FRIEND_API(void)
JS::AssertGCThingMustBeTenured(JSObject *obj)
{
    JS_ASSERT((!IsNurseryAllocable(obj->tenuredGetAllocKind()) || obj->getClass()->finalize) &&
              obj->isTenured());
}

JS_FRIEND_API(size_t)
JS::GetGCNumber()
{
    JSRuntime *rt = js::TlsPerThreadData.get()->runtimeFromMainThread();
    if (!rt)
        return 0;
    return rt->gc.number;
}

JS::AutoAssertNoGC::AutoAssertNoGC()
  : runtime(nullptr), gcNumber(0)
{
    js::PerThreadData *data = js::TlsPerThreadData.get();
    if (data) {
        /*
         * GC's from off-thread will always assert, so off-thread is implicitly
         * AutoAssertNoGC. We still need to allow AutoAssertNoGC to be used in
         * code that works from both threads, however. We also use this to
         * annotate the off thread run loops.
         */
        runtime = data->runtimeIfOnOwnerThread();
        if (runtime)
            gcNumber = runtime->gc.number;
    }
}

JS::AutoAssertNoGC::AutoAssertNoGC(JSRuntime *rt)
  : runtime(rt), gcNumber(rt->gc.number)
{
}

JS::AutoAssertNoGC::~AutoAssertNoGC()
{
    if (runtime)
        MOZ_ASSERT(gcNumber == runtime->gc.number, "GC ran inside an AutoAssertNoGC scope.");
}
#endif
