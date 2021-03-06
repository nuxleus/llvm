//===- llvm/CodeGen/SlotIndexes.h - Slot indexes representation -*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements SlotIndex and related classes. The purpuse of SlotIndex
// is to describe a position at which a register can become live, or cease to
// be live.
//
// SlotIndex is mostly a proxy for entries of the SlotIndexList, a class which
// is held is LiveIntervals and provides the real numbering. This allows
// LiveIntervals to perform largely transparent renumbering.
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_SLOTINDEXES_H
#define LLVM_CODEGEN_SLOTINDEXES_H

#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/Allocator.h"

namespace llvm {

  /// This class represents an entry in the slot index list held in the
  /// SlotIndexes pass. It should not be used directly. See the
  /// SlotIndex & SlotIndexes classes for the public interface to this
  /// information.
  class IndexListEntry {
    IndexListEntry *next, *prev;
    MachineInstr *mi;
    unsigned index;

  public:

    IndexListEntry(MachineInstr *mi, unsigned index) : mi(mi), index(index) {}

    MachineInstr* getInstr() const { return mi; }
    void setInstr(MachineInstr *mi) {
      this->mi = mi;
    }

    unsigned getIndex() const { return index; }
    void setIndex(unsigned index) {
      this->index = index;
    }
    
    IndexListEntry* getNext() { return next; }
    const IndexListEntry* getNext() const { return next; }
    void setNext(IndexListEntry *next) {
      this->next = next;
    }

    IndexListEntry* getPrev() { return prev; }
    const IndexListEntry* getPrev() const { return prev; }
    void setPrev(IndexListEntry *prev) {
      this->prev = prev;
    }
  };

  // Specialize PointerLikeTypeTraits for IndexListEntry.
  template <>
  class PointerLikeTypeTraits<IndexListEntry*> { 
  public:
    static inline void* getAsVoidPointer(IndexListEntry *p) {
      return p;
    }
    static inline IndexListEntry* getFromVoidPointer(void *p) {
      return static_cast<IndexListEntry*>(p);
    }
    enum { NumLowBitsAvailable = 3 };
  };

  /// SlotIndex - An opaque wrapper around machine indexes.
  class SlotIndex {
    friend class SlotIndexes;
    friend struct DenseMapInfo<SlotIndex>;

    enum Slot { LOAD, USE, DEF, STORE, NUM };

    PointerIntPair<IndexListEntry*, 2, unsigned> lie;

    SlotIndex(IndexListEntry *entry, unsigned slot)
      : lie(entry, slot) {}

    IndexListEntry& entry() const {
      assert(isValid() && "Attempt to compare reserved index.");
      return *lie.getPointer();
    }

    int getIndex() const {
      return entry().getIndex() | getSlot();
    }

    /// Returns the slot for this SlotIndex.
    Slot getSlot() const {
      return static_cast<Slot>(lie.getInt());
    }

    static inline unsigned getHashValue(const SlotIndex &v) {
      void *ptrVal = v.lie.getOpaqueValue();
      return (unsigned((intptr_t)ptrVal)) ^ (unsigned((intptr_t)ptrVal) >> 9);
    }

  public:
    enum {
      /// The default distance between instructions as returned by distance().
      /// This may vary as instructions are inserted and removed.
      InstrDist = 4*NUM
    };

    static inline SlotIndex getEmptyKey() {
      return SlotIndex(0, 1);
    }

    static inline SlotIndex getTombstoneKey() {
      return SlotIndex(0, 2);
    }

    /// Construct an invalid index.
    SlotIndex() : lie(0, 0) {}

    // Construct a new slot index from the given one, and set the slot.
    SlotIndex(const SlotIndex &li, Slot s)
      : lie(&li.entry(), unsigned(s)) {
      assert(lie.getPointer() != 0 &&
             "Attempt to construct index with 0 pointer.");
    }

    /// Returns true if this is a valid index. Invalid indicies do
    /// not point into an index table, and cannot be compared.
    bool isValid() const {
      return lie.getPointer();
    }

    /// Print this index to the given raw_ostream.
    void print(raw_ostream &os) const;

    /// Dump this index to stderr.
    void dump() const;

    /// Compare two SlotIndex objects for equality.
    bool operator==(SlotIndex other) const {
      return lie == other.lie;
    }
    /// Compare two SlotIndex objects for inequality.
    bool operator!=(SlotIndex other) const {
      return lie != other.lie;
    }
   
    /// Compare two SlotIndex objects. Return true if the first index
    /// is strictly lower than the second.
    bool operator<(SlotIndex other) const {
      return getIndex() < other.getIndex();
    }
    /// Compare two SlotIndex objects. Return true if the first index
    /// is lower than, or equal to, the second.
    bool operator<=(SlotIndex other) const {
      return getIndex() <= other.getIndex();
    }

    /// Compare two SlotIndex objects. Return true if the first index
    /// is greater than the second.
    bool operator>(SlotIndex other) const {
      return getIndex() > other.getIndex();
    }

    /// Compare two SlotIndex objects. Return true if the first index
    /// is greater than, or equal to, the second.
    bool operator>=(SlotIndex other) const {
      return getIndex() >= other.getIndex();
    }

    /// isSameInstr - Return true if A and B refer to the same instruction.
    static bool isSameInstr(SlotIndex A, SlotIndex B) {
      return A.lie.getPointer() == B.lie.getPointer();
    }

    /// Return the distance from this index to the given one.
    int distance(SlotIndex other) const {
      return other.getIndex() - getIndex();
    }

    /// isLoad - Return true if this is a LOAD slot.
    bool isLoad() const {
      return getSlot() == LOAD;
    }

    /// isDef - Return true if this is a DEF slot.
    bool isDef() const {
      return getSlot() == DEF;
    }

    /// isUse - Return true if this is a USE slot.
    bool isUse() const {
      return getSlot() == USE;
    }

    /// isStore - Return true if this is a STORE slot.
    bool isStore() const {
      return getSlot() == STORE;
    }

    /// Returns the base index for associated with this index. The base index
    /// is the one associated with the LOAD slot for the instruction pointed to
    /// by this index.
    SlotIndex getBaseIndex() const {
      return getLoadIndex();
    }

    /// Returns the boundary index for associated with this index. The boundary
    /// index is the one associated with the LOAD slot for the instruction
    /// pointed to by this index.
    SlotIndex getBoundaryIndex() const {
      return getStoreIndex();
    }

    /// Returns the index of the LOAD slot for the instruction pointed to by
    /// this index.
    SlotIndex getLoadIndex() const {
      return SlotIndex(&entry(), SlotIndex::LOAD);
    }    

    /// Returns the index of the USE slot for the instruction pointed to by
    /// this index.
    SlotIndex getUseIndex() const {
      return SlotIndex(&entry(), SlotIndex::USE);
    }

    /// Returns the index of the DEF slot for the instruction pointed to by
    /// this index.
    SlotIndex getDefIndex() const {
      return SlotIndex(&entry(), SlotIndex::DEF);
    }

    /// Returns the index of the STORE slot for the instruction pointed to by
    /// this index.
    SlotIndex getStoreIndex() const {
      return SlotIndex(&entry(), SlotIndex::STORE);
    }    

    /// Returns the next slot in the index list. This could be either the
    /// next slot for the instruction pointed to by this index or, if this
    /// index is a STORE, the first slot for the next instruction.
    /// WARNING: This method is considerably more expensive than the methods
    /// that return specific slots (getUseIndex(), etc). If you can - please
    /// use one of those methods.
    SlotIndex getNextSlot() const {
      Slot s = getSlot();
      if (s == SlotIndex::STORE) {
        return SlotIndex(entry().getNext(), SlotIndex::LOAD);
      }
      return SlotIndex(&entry(), s + 1);
    }

    /// Returns the next index. This is the index corresponding to the this
    /// index's slot, but for the next instruction.
    SlotIndex getNextIndex() const {
      return SlotIndex(entry().getNext(), getSlot());
    }

    /// Returns the previous slot in the index list. This could be either the
    /// previous slot for the instruction pointed to by this index or, if this
    /// index is a LOAD, the last slot for the previous instruction.
    /// WARNING: This method is considerably more expensive than the methods
    /// that return specific slots (getUseIndex(), etc). If you can - please
    /// use one of those methods.
    SlotIndex getPrevSlot() const {
      Slot s = getSlot();
      if (s == SlotIndex::LOAD) {
        return SlotIndex(entry().getPrev(), SlotIndex::STORE);
      }
      return SlotIndex(&entry(), s - 1);
    }

    /// Returns the previous index. This is the index corresponding to this
    /// index's slot, but for the previous instruction.
    SlotIndex getPrevIndex() const {
      return SlotIndex(entry().getPrev(), getSlot());
    }

  };

  /// DenseMapInfo specialization for SlotIndex.
  template <>
  struct DenseMapInfo<SlotIndex> {
    static inline SlotIndex getEmptyKey() {
      return SlotIndex::getEmptyKey();
    }
    static inline SlotIndex getTombstoneKey() {
      return SlotIndex::getTombstoneKey();
    }
    static inline unsigned getHashValue(const SlotIndex &v) {
      return SlotIndex::getHashValue(v);
    }
    static inline bool isEqual(const SlotIndex &LHS, const SlotIndex &RHS) {
      return (LHS == RHS);
    }
  };
  
  template <> struct isPodLike<SlotIndex> { static const bool value = true; };


  inline raw_ostream& operator<<(raw_ostream &os, SlotIndex li) {
    li.print(os);
    return os;
  }

  typedef std::pair<SlotIndex, MachineBasicBlock*> IdxMBBPair;

  inline bool operator<(SlotIndex V, const IdxMBBPair &IM) {
    return V < IM.first;
  }

  inline bool operator<(const IdxMBBPair &IM, SlotIndex V) {
    return IM.first < V;
  }

  struct Idx2MBBCompare {
    bool operator()(const IdxMBBPair &LHS, const IdxMBBPair &RHS) const {
      return LHS.first < RHS.first;
    }
  };

  /// SlotIndexes pass.
  ///
  /// This pass assigns indexes to each instruction.
  class SlotIndexes : public MachineFunctionPass {
  private:

    MachineFunction *mf;
    IndexListEntry *indexListHead;
    unsigned functionSize;

    typedef DenseMap<const MachineInstr*, SlotIndex> Mi2IndexMap;
    Mi2IndexMap mi2iMap;

    /// MBBRanges - Map MBB number to (start, stop) indexes.
    SmallVector<std::pair<SlotIndex, SlotIndex>, 8> MBBRanges;

    /// Idx2MBBMap - Sorted list of pairs of index of first instruction
    /// and MBB id.
    SmallVector<IdxMBBPair, 8> idx2MBBMap;

    // IndexListEntry allocator.
    BumpPtrAllocator ileAllocator;

    IndexListEntry* createEntry(MachineInstr *mi, unsigned index) {
      IndexListEntry *entry =
        static_cast<IndexListEntry*>(
          ileAllocator.Allocate(sizeof(IndexListEntry),
          alignOf<IndexListEntry>()));

      new (entry) IndexListEntry(mi, index);

      return entry;
    }

    void initList() {
      assert(indexListHead == 0 && "Zero entry non-null at initialisation.");
      indexListHead = createEntry(0, ~0U);
      indexListHead->setNext(0);
      indexListHead->setPrev(indexListHead);
    }

    void clearList() {
      indexListHead = 0;
      ileAllocator.Reset();
    }

    IndexListEntry* getTail() {
      assert(indexListHead != 0 && "Call to getTail on uninitialized list.");
      return indexListHead->getPrev();
    }

    const IndexListEntry* getTail() const {
      assert(indexListHead != 0 && "Call to getTail on uninitialized list.");
      return indexListHead->getPrev();
    }

    // Returns true if the index list is empty.
    bool empty() const { return (indexListHead == getTail()); }

    IndexListEntry* front() {
      assert(!empty() && "front() called on empty index list.");
      return indexListHead;
    }

    const IndexListEntry* front() const {
      assert(!empty() && "front() called on empty index list.");
      return indexListHead;
    }

    IndexListEntry* back() {
      assert(!empty() && "back() called on empty index list.");
      return getTail()->getPrev();
    }

    const IndexListEntry* back() const {
      assert(!empty() && "back() called on empty index list.");
      return getTail()->getPrev();
    }

    /// Insert a new entry before itr.
    void insert(IndexListEntry *itr, IndexListEntry *val) {
      assert(itr != 0 && "itr should not be null.");
      IndexListEntry *prev = itr->getPrev();
      val->setNext(itr);
      val->setPrev(prev);
      
      if (itr != indexListHead) {
        prev->setNext(val);
      }
      else {
        indexListHead = val;
      }
      itr->setPrev(val);
    }

    /// Push a new entry on to the end of the list.
    void push_back(IndexListEntry *val) {
      insert(getTail(), val);
    }

    /// Renumber locally after inserting newEntry.
    void renumberIndexes(IndexListEntry *newEntry);

  public:
    static char ID;

    SlotIndexes() : MachineFunctionPass(ID), indexListHead(0) {
      initializeSlotIndexesPass(*PassRegistry::getPassRegistry());
    }

    virtual void getAnalysisUsage(AnalysisUsage &au) const;
    virtual void releaseMemory(); 

    virtual bool runOnMachineFunction(MachineFunction &fn);

    /// Dump the indexes.
    void dump() const;

    /// Renumber the index list, providing space for new instructions.
    void renumberIndexes();

    /// Returns the zero index for this analysis.
    SlotIndex getZeroIndex() {
      assert(front()->getIndex() == 0 && "First index is not 0?");
      return SlotIndex(front(), 0);
    }

    /// Returns the base index of the last slot in this analysis.
    SlotIndex getLastIndex() {
      return SlotIndex(back(), 0);
    }

    /// Returns the invalid index marker for this analysis.
    SlotIndex getInvalidIndex() {
      return getZeroIndex();
    }

    /// Returns the distance between the highest and lowest indexes allocated
    /// so far.
    unsigned getIndexesLength() const {
      assert(front()->getIndex() == 0 &&
             "Initial index isn't zero?");

      return back()->getIndex();
    }

    /// Returns the number of instructions in the function.
    unsigned getFunctionSize() const {
      return functionSize;
    }

    /// Returns true if the given machine instr is mapped to an index,
    /// otherwise returns false.
    bool hasIndex(const MachineInstr *instr) const {
      return (mi2iMap.find(instr) != mi2iMap.end());
    }

    /// Returns the base index for the given instruction.
    SlotIndex getInstructionIndex(const MachineInstr *instr) const {
      Mi2IndexMap::const_iterator itr = mi2iMap.find(instr);
      assert(itr != mi2iMap.end() && "Instruction not found in maps.");
      return itr->second;
    }

    /// Returns the instruction for the given index, or null if the given
    /// index has no instruction associated with it.
    MachineInstr* getInstructionFromIndex(SlotIndex index) const {
      return index.isValid() ? index.entry().getInstr() : 0;
    }

    /// Returns the next non-null index.
    SlotIndex getNextNonNullIndex(SlotIndex index) {
      SlotIndex nextNonNull = index.getNextIndex();

      while (&nextNonNull.entry() != getTail() &&
             getInstructionFromIndex(nextNonNull) == 0) {
        nextNonNull = nextNonNull.getNextIndex();
      }

      return nextNonNull;
    }

    /// Return the (start,end) range of the given basic block number.
    const std::pair<SlotIndex, SlotIndex> &
    getMBBRange(unsigned Num) const {
      return MBBRanges[Num];
    }

    /// Return the (start,end) range of the given basic block.
    const std::pair<SlotIndex, SlotIndex> &
    getMBBRange(const MachineBasicBlock *MBB) const {
      return getMBBRange(MBB->getNumber());
    }

    /// Returns the first index in the given basic block number.
    SlotIndex getMBBStartIdx(unsigned Num) const {
      return getMBBRange(Num).first;
    }

    /// Returns the first index in the given basic block.
    SlotIndex getMBBStartIdx(const MachineBasicBlock *mbb) const {
      return getMBBRange(mbb).first;
    }

    /// Returns the last index in the given basic block number.
    SlotIndex getMBBEndIdx(unsigned Num) const {
      return getMBBRange(Num).second;
    }

    /// Returns the last index in the given basic block.
    SlotIndex getMBBEndIdx(const MachineBasicBlock *mbb) const {
      return getMBBRange(mbb).second;
    }

    /// Returns the basic block which the given index falls in.
    MachineBasicBlock* getMBBFromIndex(SlotIndex index) const {
      if (MachineInstr *MI = getInstructionFromIndex(index))
        return MI->getParent();
      SmallVectorImpl<IdxMBBPair>::const_iterator I =
        std::lower_bound(idx2MBBMap.begin(), idx2MBBMap.end(), index);
      // Take the pair containing the index
      SmallVectorImpl<IdxMBBPair>::const_iterator J =
        ((I != idx2MBBMap.end() && I->first > index) ||
         (I == idx2MBBMap.end() && idx2MBBMap.size()>0)) ? (I-1): I;

      assert(J != idx2MBBMap.end() && J->first <= index &&
             index < getMBBEndIdx(J->second) &&
             "index does not correspond to an MBB");
      return J->second;
    }

    bool findLiveInMBBs(SlotIndex start, SlotIndex end,
                        SmallVectorImpl<MachineBasicBlock*> &mbbs) const {
      SmallVectorImpl<IdxMBBPair>::const_iterator itr =
        std::lower_bound(idx2MBBMap.begin(), idx2MBBMap.end(), start);
      bool resVal = false;

      while (itr != idx2MBBMap.end()) {
        if (itr->first >= end)
          break;
        mbbs.push_back(itr->second);
        resVal = true;
        ++itr;
      }
      return resVal;
    }

    /// Returns the MBB covering the given range, or null if the range covers
    /// more than one basic block.
    MachineBasicBlock* getMBBCoveringRange(SlotIndex start, SlotIndex end) const {

      assert(start < end && "Backwards ranges not allowed.");

      SmallVectorImpl<IdxMBBPair>::const_iterator itr =
        std::lower_bound(idx2MBBMap.begin(), idx2MBBMap.end(), start);

      if (itr == idx2MBBMap.end()) {
        itr = prior(itr);
        return itr->second;
      }

      // Check that we don't cross the boundary into this block.
      if (itr->first < end)
        return 0;

      itr = prior(itr);

      if (itr->first <= start)
        return itr->second;

      return 0;
    }

    /// Insert the given machine instruction into the mapping. Returns the
    /// assigned index.
    SlotIndex insertMachineInstrInMaps(MachineInstr *mi) {
      assert(mi2iMap.find(mi) == mi2iMap.end() && "Instr already indexed.");
      // Numbering DBG_VALUE instructions could cause code generation to be
      // affected by debug information.
      assert(!mi->isDebugValue() && "Cannot number DBG_VALUE instructions.");

      MachineBasicBlock *mbb = mi->getParent();

      assert(mbb != 0 && "Instr must be added to function.");

      MachineBasicBlock::iterator miItr(mi);
      IndexListEntry *newEntry;
      // Get previous index, considering that not all instructions are indexed.
      IndexListEntry *prevEntry;
      for (;;) {
        // If mi is at the mbb beginning, get the prev index from the mbb.
        if (miItr == mbb->begin()) {
          prevEntry = &getMBBStartIdx(mbb).entry();
          break;
        }
        // Otherwise rewind until we find a mapped instruction.
        Mi2IndexMap::const_iterator itr = mi2iMap.find(--miItr);
        if (itr != mi2iMap.end()) {
          prevEntry = &itr->second.entry();
          break;
        }
      }

      // Get next entry from previous entry.
      IndexListEntry *nextEntry = prevEntry->getNext();

      // Get a number for the new instr, or 0 if there's no room currently.
      // In the latter case we'll force a renumber later.
      unsigned dist = ((nextEntry->getIndex() - prevEntry->getIndex())/2) & ~3u;
      unsigned newNumber = prevEntry->getIndex() + dist;

      // Insert a new list entry for mi.
      newEntry = createEntry(mi, newNumber);
      insert(nextEntry, newEntry);

      // Renumber locally if we need to.
      if (dist == 0)
        renumberIndexes(newEntry);

      SlotIndex newIndex(newEntry, SlotIndex::LOAD);
      mi2iMap.insert(std::make_pair(mi, newIndex));
      return newIndex;
    }

    /// Remove the given machine instruction from the mapping.
    void removeMachineInstrFromMaps(MachineInstr *mi) {
      // remove index -> MachineInstr and
      // MachineInstr -> index mappings
      Mi2IndexMap::iterator mi2iItr = mi2iMap.find(mi);
      if (mi2iItr != mi2iMap.end()) {
        IndexListEntry *miEntry(&mi2iItr->second.entry());        
        assert(miEntry->getInstr() == mi && "Instruction indexes broken.");
        // FIXME: Eventually we want to actually delete these indexes.
        miEntry->setInstr(0);
        mi2iMap.erase(mi2iItr);
      }
    }

    /// ReplaceMachineInstrInMaps - Replacing a machine instr with a new one in
    /// maps used by register allocator.
    void replaceMachineInstrInMaps(MachineInstr *mi, MachineInstr *newMI) {
      Mi2IndexMap::iterator mi2iItr = mi2iMap.find(mi);
      if (mi2iItr == mi2iMap.end())
        return;
      SlotIndex replaceBaseIndex = mi2iItr->second;
      IndexListEntry *miEntry(&replaceBaseIndex.entry());
      assert(miEntry->getInstr() == mi &&
             "Mismatched instruction in index tables.");
      miEntry->setInstr(newMI);
      mi2iMap.erase(mi2iItr);
      mi2iMap.insert(std::make_pair(newMI, replaceBaseIndex));
    }

    /// Add the given MachineBasicBlock into the maps.
    void insertMBBInMaps(MachineBasicBlock *mbb) {
      MachineFunction::iterator nextMBB =
        llvm::next(MachineFunction::iterator(mbb));
      IndexListEntry *startEntry = createEntry(0, 0);
      IndexListEntry *stopEntry = createEntry(0, 0);
      IndexListEntry *nextEntry = 0;

      if (nextMBB == mbb->getParent()->end()) {
        nextEntry = getTail();
      } else {
        nextEntry = &getMBBStartIdx(nextMBB).entry();
      }

      insert(nextEntry, startEntry);
      insert(nextEntry, stopEntry);

      SlotIndex startIdx(startEntry, SlotIndex::LOAD);
      SlotIndex endIdx(nextEntry, SlotIndex::LOAD);

      assert(unsigned(mbb->getNumber()) == MBBRanges.size() &&
             "Blocks must be added in order");
      MBBRanges.push_back(std::make_pair(startIdx, endIdx));

      idx2MBBMap.push_back(IdxMBBPair(startIdx, mbb));

      renumberIndexes();
      std::sort(idx2MBBMap.begin(), idx2MBBMap.end(), Idx2MBBCompare());
    }

  };


  // Specialize IntervalMapInfo for half-open slot index intervals.
  template <typename> struct IntervalMapInfo;
  template <> struct IntervalMapInfo<SlotIndex> {
    static inline bool startLess(const SlotIndex &x, const SlotIndex &a) {
      return x < a;
    }
    static inline bool stopLess(const SlotIndex &b, const SlotIndex &x) {
      return b <= x;
    }
    static inline bool adjacent(const SlotIndex &a, const SlotIndex &b) {
      return a == b;
    }
  };

}

#endif // LLVM_CODEGEN_LIVEINDEX_H 
