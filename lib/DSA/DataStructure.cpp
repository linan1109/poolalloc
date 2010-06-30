//===- DataStructure.cpp - Implement the core data structure analysis -----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the core data structure functionality.
//
//===----------------------------------------------------------------------===//

#include "dsa/DSGraphTraits.h"
#include "dsa/DataStructure.h"
#include "dsa/DSGraph.h"
#include "dsa/DSSupport.h"
#include "dsa/DSNode.h"
#include "llvm/Constants.h"
#include "llvm/Function.h"
#include "llvm/GlobalVariable.h"
#include "llvm/Instructions.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Assembly/Writer.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"

#include <iostream>
#include <algorithm>
using namespace llvm;

#define COLLAPSE_ARRAYS_AGGRESSIVELY 0
namespace {
  STATISTIC (NumFolds, "Number of nodes completely folded");
  STATISTIC (NumNodeAllocated  , "Number of nodes allocated");
}

/// isForwarding - Return true if this NodeHandle is forwarding to another
/// one.
bool DSNodeHandle::isForwarding() const {
  return N && N->isForwarding();
}

DSNode *DSNodeHandle::HandleForwarding() const {
  assert(N->isForwarding() && "Can only be invoked if forwarding!");
  DEBUG(
        { //assert not looping
          DSNode* NH = N;
          svset<DSNode*> seen;
          while(NH && NH->isForwarding()) {
            assert(seen.find(NH) == seen.end() && "Loop detected");
            seen.insert(NH);
            NH = NH->ForwardNH.N;
          }
        }
        );
  // Handle node forwarding here!
  DSNode *Next = N->ForwardNH.getNode();  // Cause recursive shrinkage
  Offset += N->ForwardNH.getOffset();

  if (--N->NumReferrers == 0) {
    // Removing the last referrer to the node, sever the forwarding link
    N->stopForwarding();
  }

  N = Next;
  N->NumReferrers++;

  if (N->getSize() <= Offset) {
    assert(N->getSize() <= 1 && "Forwarded to shrunk but not collapsed node?");
    Offset = 0;
  }
  return N;
}

//===----------------------------------------------------------------------===//
// DSScalarMap Implementation
//===----------------------------------------------------------------------===//

DSNodeHandle &DSScalarMap::AddGlobal(const GlobalValue *GV) {
  assert(GV);
  assert(ValueMap.count(GV) == 0 && "GV already exists!");

  // If the node doesn't exist, check to see if it's a global that is
  // equated to another global in the program.
  EquivalenceClasses<const GlobalValue*>::iterator ECI = GlobalECs.findValue(GV);
  if (ECI != GlobalECs.end()) {
    const GlobalValue *Leader = *GlobalECs.findLeader(ECI);
    if (Leader != GV) {
      GV = Leader;
      iterator I = ValueMap.find(GV);
      if (I != ValueMap.end())
        return I->second;
    }
  }

  // Okay, this is either not an equivalenced global or it is the leader, it
  // will be inserted into the scalar map now.
  GlobalSet.insert(GV);

  return ValueMap.insert(std::make_pair(GV, DSNodeHandle())).first->second;
}

/// spliceFrom - Copy all entries from RHS, then clear RHS.
///
void DSScalarMap::spliceFrom(DSScalarMap &RHS) {
  // Special case if this is empty.
  if (ValueMap.empty()) {
    ValueMap.swap(RHS.ValueMap);
    GlobalSet.swap(RHS.GlobalSet);
  } else {
    GlobalSet.insert(RHS.GlobalSet.begin(), RHS.GlobalSet.end());
    for (ValueMapTy::iterator I = RHS.ValueMap.begin(), E = RHS.ValueMap.end();
         I != E; ++I)
      ValueMap[I->first].mergeWith(I->second);
    RHS.ValueMap.clear();
  }
}

//===----------------------------------------------------------------------===//
// DSNode Implementation
//===----------------------------------------------------------------------===//

DSNode::DSNode(DSGraph *G)
: NumReferrers(0), Size(0), ParentGraph(G), NodeType(0) {
  // Add the type entry if it is specified...
  if (G) G->addNode(this);
  ++NumNodeAllocated;
}

// DSNode copy constructor... do not copy over the referrers list!
DSNode::DSNode(const DSNode &N, DSGraph *G, bool NullLinks)
  : NumReferrers(0), Size(N.Size), ParentGraph(G), TyMap(N.TyMap),
    Globals(N.Globals), NodeType(N.NodeType) {
  if (!NullLinks) Links = N.Links;
  G->addNode(this);
  ++NumNodeAllocated;
}

DSNode::~DSNode() {
  dropAllReferences();
  assert(hasNoReferrers() && "Referrers to dead node exist!");
}

void DSNode::assertOK() const {
//  assert(((Ty && Ty->getTypeID() != Type::VoidTyID) ||
//         ((!Ty || Ty->getTypeID() == Type::VoidTyID) && (Size == 0 ||
//         (NodeType & DSNode::ArrayNode)))) &&
//         "Node not OK!");

  assert(ParentGraph && "Node has no parent?");
  const DSScalarMap &SM = ParentGraph->getScalarMap();
  for (globals_iterator ii = globals_begin(), ee = globals_end();
       ii != ee; ++ii) {
    assert(SM.global_count(*ii));
    assert(SM.find(*ii)->second.getNode() == this);
  }
}

/// forwardNode - Mark this node as being obsolete, and all references to it
/// should be forwarded to the specified node and offset.
///
void DSNode::forwardNode(DSNode *To, unsigned Offset) {
  assert(this != To && "Cannot forward a node to itself!");
  assert(ForwardNH.isNull() && "Already forwarding from this node!");
  if (To->Size <= 1) Offset = 0;
  assert((Offset < To->Size || (Offset == To->Size && Offset == 0)) &&
         "Forwarded offset is wrong!");
  ForwardNH.setTo(To, Offset);
  NodeType = DeadNode;
  Size = 0;

  DSNodeHandle ToNH(To,Offset);

  //Move the Links
  for (LinkMapTy::iterator ii = Links.begin(), ee = Links.end();
       ii != ee; ++ii) {
    if (!ii->second.isNull()) {
      // Compute the offset into the current node at which to
      // merge this link.  In the common case, this is a linear
      // relation to the offset in the original node (with
      // wrapping), but if the current node gets collapsed due to
      // recursive merging, we must make sure to merge in all remaining
      // links at offset zero.
      unsigned MergeOffset = 0;
      if (ToNH.getNode()->getSize() != 1)
        MergeOffset = (ii->first + Offset) % ToNH.getNode()->getSize();
      ToNH.getNode()->addEdgeTo(MergeOffset, ii->second);
    }
  }
  Links.clear();

  // Remove this node from the parent graph's Nodes list.
  ParentGraph->unlinkNode(this);
  ParentGraph = 0;
}

// addGlobal - Add an entry for a global value to the Globals list.  This also
// marks the node with the 'G' flag if it does not already have it.
//
void DSNode::addGlobal(const GlobalValue *GV) {
  // First, check to make sure this is the leader if the global is in an
  // equivalence class.
  GV = getParentGraph()->getScalarMap().getLeaderForGlobal(GV);

  Globals.insert(GV);
  setGlobalMarker();
}

void DSNode::addFunction(const Function* F) {
  addGlobal(F);
}

// removeGlobal - Remove the specified global that is explicitly in the globals
// list.
void DSNode::removeGlobal(const GlobalValue *GV) {
  assert (Globals.count(GV) && "Global not in Node!");
  Globals.erase(GV);
}

/// foldNodeCompletely - If we determine that this node has some funny
/// behavior happening to it that we cannot represent, we fold it down to a
/// single, completely pessimistic, node.  This node is represented as a
/// single byte with a single TypeEntry of "void".
///
void DSNode::foldNodeCompletely() {
  if (isNodeCompletelyFolded()) return;  // If this node is already folded...

//  assert(0 && "Folding is happening");

  ++NumFolds;

  //Collapsed nodes don't really need a type
  TyMap.clear();

  // If this node has a size that is <= 1, we don't need to create a forwarding
  // node.
  if (getSize() <= 1) {
    setCollapsedMarker();
    Size = 1;
    assert(Links.size() <= 1 && "Size is 1, but has more links?");
  } else {
    // Create the node we are going to forward to.  This is required because
    // some referrers may have an offset that is > 0.  By forcing them to
    // forward, the forwarder has the opportunity to correct the offset.
    DSNode *DestNode = new DSNode(ParentGraph);
    DestNode->NodeType = NodeType;
    DestNode->setCollapsedMarker();
    DestNode->Size = 1;
    DestNode->Globals.swap(Globals);

    // Start forwarding to the destination node...
    forwardNode(DestNode, 0);

  }
}

/// isNodeCompletelyFolded - Return true if this node has been completely
/// folded down to something that can never be expanded, effectively losing
/// all of the field sensitivity that may be present in the node.
///
bool DSNode::isNodeCompletelyFolded() const {
  return  isCollapsedNode();
}

/// addFullGlobalsList - Compute the full set of global values that are
/// represented by this node.  Unlike getGlobalsList(), this requires fair
/// amount of work to compute, so don't treat this method call as free.
void DSNode::addFullGlobalsList(std::vector<const GlobalValue*> &List) const {
  if (globals_begin() == globals_end()) return;

  EquivalenceClasses<const GlobalValue*> &EC = getParentGraph()->getGlobalECs();

  for (globals_iterator I = globals_begin(), E = globals_end(); I != E; ++I) {
    EquivalenceClasses<const GlobalValue*>::iterator ECI = EC.findValue(*I);
    if (ECI == EC.end())
      List.push_back(*I);
    else
      List.insert(List.end(), EC.member_begin(ECI), EC.member_end());
  }
}

/// addFullFunctionList - Identical to addFullGlobalsList, but only return the
/// functions in the full list.
void DSNode::addFullFunctionList(std::vector<const Function*> &List) const {
  if (globals_begin() == globals_end()) return;

  EquivalenceClasses<const GlobalValue*> &EC = getParentGraph()->getGlobalECs();

  for (globals_iterator I = globals_begin(), E = globals_end(); I != E; ++I) {
    EquivalenceClasses<const GlobalValue*>::iterator ECI = EC.findValue(*I);
    if (ECI == EC.end()) {
      if (const Function *F = dyn_cast<Function>(*I))
        List.push_back(F);
    } else {
      for (EquivalenceClasses<const GlobalValue*>::member_iterator MI =
             EC.member_begin(ECI), E = EC.member_end(); MI != E; ++MI)
        if (const Function *F = dyn_cast<Function>(*MI))
          List.push_back(F);
    }
  }
}

/// mergeTypeInfo - This method merges the specified type into the current node
/// at the specified offset.  This may update the current node's type record if
/// this gives more information to the node, it may do nothing to the node if
/// this information is already known, or it may merge the node completely (and
/// return true) if the information is incompatible with what is already known.
///
/// This method returns true if the node is completely folded, otherwise false.
///
void DSNode::mergeTypeInfo(const Type *NewTy, unsigned Offset) {

  if (!NewTy || NewTy->isVoidTy()) return;

  if (isCollapsedNode()) return;
  if (isArrayNode()) Offset %= getSize();

  if (Offset >= getSize()) growSize(Offset+1);

  TyMap[Offset] = getParentGraph()->getTypeSS().getOrCreate(TyMap[Offset], NewTy);
  assert(TyMap[Offset]);
}

void DSNode::mergeTypeInfo(const TyMapTy::mapped_type TyIt, unsigned Offset) {
  if (isCollapsedNode()) return;
  if (isArrayNode()) Offset %= getSize();

  if (Offset >= getSize())
    growSize(Offset + 1);

  if (!TyMap[Offset])
    TyMap[Offset] = TyIt;
  if (TyIt) {
    svset<const Type*> S(*TyMap[Offset]);
    S.insert(TyIt->begin(), TyIt->end());
    TyMap[Offset] = getParentGraph()->getTypeSS().getOrCreate(S);
  }
  assert(TyMap[Offset]);
}

void DSNode::mergeTypeInfo(const DSNode* DN, unsigned Offset) {
  if (isCollapsedNode()) return;

  for (TyMapTy::const_iterator ii = DN->TyMap.begin(), ee = DN->TyMap.end();
       ii != ee; ++ii)
    mergeTypeInfo(ii->second, ii->first + Offset);
}

/// addEdgeTo - Add an edge from the current node to the specified node.  This
/// can cause merging of nodes in the graph.
///
void DSNode::addEdgeTo(unsigned Offset, const DSNodeHandle &NH) {
  if (NH.isNull()) return;       // Nothing to do

  if (isNodeCompletelyFolded())
    Offset = 0;

  DSNodeHandle &ExistingEdge = getLink(Offset);
  if (!ExistingEdge.isNull()) {
    // Merge the two nodes...
    ExistingEdge.mergeWith(NH);
  } else {                             // No merging to perform...
    setLink(Offset, NH);               // Just force a link in there...
  }
}


void DSNode::mergeGlobals(const DSNode &RHS) {
  Globals.insert(RHS.Globals.begin(), RHS.Globals.end());
}

static unsigned gcd(unsigned m, unsigned n) {
  if (m < n) return gcd(n,m);
  unsigned r = m % n;
  if (r == 0) {
    return n;
  } else {
    return gcd(n, r);
  }
}

// MergeNodes - Helper function for DSNode::mergeWith().
// This function does the hard work of merging two nodes, CurNodeH
// and NH after filtering out trivial cases and making sure that
// CurNodeH.offset >= NH.offset.
//
// ***WARNING***
// Since merging may cause either node to go away, we must always
// use the node-handles to refer to the nodes.  These node handles are
// automatically updated during merging, so will always provide access
// to the correct node after a merge.
//
void DSNode::MergeNodes(DSNodeHandle& CurNodeH, DSNodeHandle& NH) {
  assert(CurNodeH.getOffset() >= NH.getOffset() &&
         "This should have been enforced in the caller.");
  assert(CurNodeH.getNode()->getParentGraph()==NH.getNode()->getParentGraph() &&
         "Cannot merge two nodes that are not in the same graph!");

  // Now we know that Offset >= NH.Offset, so convert it so our "Offset" (with
  // respect to NH.Offset) is now zero.  NOffset is the distance from the base
  // of our object that N starts from.
  //
  unsigned NOffset = CurNodeH.getOffset()-NH.getOffset();
  unsigned NSize = NH.getNode()->getSize();

  // If the two nodes are of different size, and the smaller node has the array
  // bit set, collapse!
  if (NSize != CurNodeH.getNode()->getSize()) {
#if COLLAPSE_ARRAYS_AGGRESSIVELY
    if (NSize < CurNodeH.getNode()->getSize()) {
      if (NH.getNode()->isArray())
        NH.getNode()->foldNodeCompletely();
    } else if (CurNodeH.getNode()->isArray()) {
      NH.getNode()->foldNodeCompletely();
    }
#endif
  }

  // If we are merging a node with a completely folded node, then both nodes are
  // now completely folded.
  //
  if (CurNodeH.getNode()->isNodeCompletelyFolded()) {
    if (!NH.getNode()->isNodeCompletelyFolded()) {
      NH.getNode()->foldNodeCompletely();
      assert(NH.getNode() && NH.getOffset() == 0 &&
             "folding did not make offset 0?");
      NOffset = NH.getOffset();
      NSize = NH.getNode()->getSize();
      assert(NOffset == 0 && NSize == 1);
    }
  } else if (NH.getNode()->isNodeCompletelyFolded()) {
    CurNodeH.getNode()->foldNodeCompletely();
    assert(CurNodeH.getNode() && CurNodeH.getOffset() == 0 &&
           "folding did not make offset 0?");
    NSize = NH.getNode()->getSize();
    NOffset = NH.getOffset();
    assert(NOffset == 0 && NSize == 1);
  } 

  DSNode *N = NH.getNode();
  if (CurNodeH.getNode() == N || N == 0) return;
  assert(!CurNodeH.getNode()->isDeadNode());

  // Merge the type entries of the two nodes together...
  CurNodeH.getNode()->mergeTypeInfo(NH.getNode(), NOffset);
  if (NH.getNode()->getSize() + NOffset > CurNodeH.getNode()->getSize())
    CurNodeH.getNode()->growSize(NH.getNode()->getSize() + NOffset);
  assert(!CurNodeH.getNode()->isDeadNode());



  // Merge the NodeType information.
  CurNodeH.getNode()->NodeType |= N->NodeType;

  // Start forwarding to the new node!
  N->forwardNode(CurNodeH.getNode(), NOffset);
  assert(!CurNodeH.getNode()->isDeadNode());

  // Make all of the outgoing links of N now be outgoing links of CurNodeH.
  //
  for (LinkMapTy::iterator ii = N->Links.begin(), ee = N->Links.end();
       ii != ee; ++ii)
    if (ii->second.getNode()) {
      // Compute the offset into the current node at which to
      // merge this link.  In the common case, this is a linear
      // relation to the offset in the original node (with
      // wrapping), but if the current node gets collapsed due to
      // recursive merging, we must make sure to merge in all remaining
      // links at offset zero.
      unsigned MergeOffset = 0;
       DSNode *CN = CurNodeH.getNode();
      if (CN->Size != 1)
        MergeOffset = (ii->first + NOffset) % CN->getSize();
      CN->addEdgeTo(MergeOffset, ii->second);
    }

  // Now that there are no outgoing edges, all of the Links are dead.
  N->Links.clear();

  // Merge the globals list...
  CurNodeH.getNode()->mergeGlobals(*N);

  // Delete the globals from the old node...
  N->Globals.clear();
}


/// mergeWith - Merge this node and the specified node, moving all links to and
/// from the argument node into the current node, deleting the node argument.
/// Offset indicates what offset the specified node is to be merged into the
/// current node.
///
/// The specified node may be a null pointer (in which case, we update it to
/// point to this node).
///
void DSNode::mergeWith(const DSNodeHandle &NH, unsigned Offset) {
  //DOUT << "mergeWith: " << this << " becomes " << NH.getNode() << "\n";
  DSNode *N = NH.getNode();
  if (N == this && NH.getOffset() == Offset)
    return;  // Noop

  // If the RHS is a null node, make it point to this node!
  if (N == 0) {
    NH.mergeWith(DSNodeHandle(this, Offset));
    return;
  }

  assert(!N->isDeadNode() && !isDeadNode());
  assert(!hasNoReferrers() && "Should not try to fold a useless node!");

  if (N == this) {
    // We cannot merge two pieces of the same node together, collapse the node
    // completely.
    DEBUG(errs() << "Attempting to merge two chunks of the same node together!\n");
    foldNodeCompletely();
    return;
  }

  // If both nodes are not at offset 0, make sure that we are merging the node
  // at an later offset into the node with the zero offset.
  //
  if (Offset < NH.getOffset()) {
    N->mergeWith(DSNodeHandle(this, Offset), NH.getOffset());
    return;
  } else if (Offset == NH.getOffset() && getSize() < N->getSize()) {
    // If the offsets are the same, merge the smaller node into the bigger node
    N->mergeWith(DSNodeHandle(this, Offset), NH.getOffset());
    return;
  }

  // Ok, now we can merge the two nodes.  Use a static helper that works with
  // two node handles, since "this" may get merged away at intermediate steps.
  DSNodeHandle CurNodeH(this, Offset);
  DSNodeHandle NHCopy(NH);
  if (CurNodeH.getOffset() >= NHCopy.getOffset())
    DSNode::MergeNodes(CurNodeH, NHCopy);
  else
    DSNode::MergeNodes(NHCopy, CurNodeH);
}

void DSNode::cleanEdges() {
  //get rid of any type edge pointing to the null type
  for (type_iterator ii = type_begin(); ii != type_end(); ) {
    if (ii->second)
      ++ii;
    else {
      type_iterator backup = ii;
      ++backup;
      TyMap.erase(ii);
      ii = backup;
    }
  }
  //get rid of any node edge pointing to nothing
  for (edge_iterator ii = edge_begin(); ii != edge_end(); ) {
    if (ii->second.isNull()) {
      edge_iterator backup = ii;
      ++backup;
      Links.erase(ii);
      ii = backup;
    } else
      ++ii;
  }
}

//===----------------------------------------------------------------------===//
// ReachabilityCloner Implementation
//===----------------------------------------------------------------------===//

DSNodeHandle ReachabilityCloner::getClonedNH(const DSNodeHandle &SrcNH) {
  if (SrcNH.isNull()) return DSNodeHandle();
  const DSNode *SN = SrcNH.getNode();

  DSNodeHandle &NH = NodeMap[SN];
  if (!NH.isNull()) { // Node already mapped?
    DSNode *NHN = NH.getNode();
    return DSNodeHandle(NHN, NH.getOffset() + SrcNH.getOffset());
  }
  
  // If SrcNH has globals and the destination graph has one of the same globals,
  // merge this node with the destination node, which is much more efficient.
  if (SN->globals_begin() != SN->globals_end()) {
    DSScalarMap &DestSM = Dest->getScalarMap();
    for (DSNode::globals_iterator I = SN->globals_begin(),E = SN->globals_end();
         I != E; ++I) {
      const GlobalValue *GV = *I;
      DSScalarMap::iterator GI = DestSM.find(GV);
      if (GI != DestSM.end() && !GI->second.isNull()) {
        // We found one, use merge instead!
        merge(GI->second, Src->getNodeForValue(GV));
        assert(!NH.isNull() && "Didn't merge node!");
        DSNode *NHN = NH.getNode();
        return DSNodeHandle(NHN, NH.getOffset()+SrcNH.getOffset());
      }
    }
  }

  if (!createDest) return DSNodeHandle(0,0);

  DSNode *DN = new DSNode(*SN, Dest, true /* Null out all links */);
  DN->maskNodeTypes(BitsToKeep);
  NH = DN;
  //DOUT << "getClonedNH: " << SN << " becomes " << DN << "\n";

  // Next, recursively clone all outgoing links as necessary.  Note that
  // adding these links can cause the node to collapse itself at any time, and
  // the current node may be merged with arbitrary other nodes.  For this
  // reason, we must always go through NH.
  DN = 0;
  for (DSNode::const_edge_iterator ii = SN->edge_begin(), ee = SN->edge_end();
       ii != ee; ++ii) {
    const DSNodeHandle &SrcEdge = ii->second;
    if (!SrcEdge.isNull()) {
      const DSNodeHandle &DestEdge = getClonedNH(SrcEdge);
      // Compute the offset into the current node at which to
      // merge this link.  In the common case, this is a linear
      // relation to the offset in the original node (with
      // wrapping), but if the current node gets collapsed due to
      // recursive merging, we must make sure to merge in all remaining
      // links at offset zero.
      unsigned MergeOffset = 0;
      DSNode *CN = NH.getNode();
      if (CN->getSize() != 1)
        MergeOffset = (ii->first + NH.getOffset()) % CN->getSize();
      CN->addEdgeTo(MergeOffset, DestEdge);
    }
  }

  // If this node contains any globals, make sure they end up in the scalar
  // map with the correct offset.
  for (DSNode::globals_iterator I = SN->globals_begin(), E = SN->globals_end();
       I != E; ++I) {
    const GlobalValue *GV = *I;
    const DSNodeHandle &SrcGNH = Src->getNodeForValue(GV);
    DSNodeHandle &DestGNH = NodeMap[SrcGNH.getNode()];
    assert(DestGNH.getNode() == NH.getNode() &&"Global mapping inconsistent");
    Dest->getNodeForValue(GV).mergeWith(DSNodeHandle(DestGNH.getNode(),
                                        DestGNH.getOffset()+SrcGNH.getOffset()));
  }
  NH.getNode()->mergeGlobals(*SN);

  return DSNodeHandle(NH.getNode(), NH.getOffset()+SrcNH.getOffset());
}

void ReachabilityCloner::merge(const DSNodeHandle &NH,
                               const DSNodeHandle &SrcNH) {
  if (SrcNH.isNull()) return;  // Noop
  if (NH.isNull()) {
    // If there is no destination node, just clone the source and assign the
    // destination node to be it.
    NH.mergeWith(getClonedNH(SrcNH));
    return;
  }

  // Okay, at this point, we know that we have both a destination and a source
  // node that need to be merged.  Check to see if the source node has already
  // been cloned.
  const DSNode *SN = SrcNH.getNode();
  DSNodeHandle &SCNH = NodeMap[SN];  // SourceClonedNodeHandle
  if (!SCNH.isNull()) {   // Node already cloned?
    DSNode *SCNHN = SCNH.getNode();
    NH.mergeWith(DSNodeHandle(SCNHN,
                              SCNH.getOffset()+SrcNH.getOffset()));
    return;  // Nothing to do!
  }

  // Okay, so the source node has not already been cloned.  Instead of creating
  // a new DSNode, only to merge it into the one we already have, try to perform
  // the merge in-place.  The only case we cannot handle here is when the offset
  // into the existing node is less than the offset into the virtual node we are
  // merging in.  In this case, we have to extend the existing node, which
  // requires an allocation anyway.
  DSNode *DN = NH.getNode();   // Make sure the Offset is up-to-date
  if (NH.getOffset() >= SrcNH.getOffset()) {
    if (!DN->isNodeCompletelyFolded()) {
      // Make sure the destination node is folded if the source node is folded.
      if (SN->isNodeCompletelyFolded()) {
        DN->foldNodeCompletely();
        DN = NH.getNode();
      } else if (SN->getSize() != DN->getSize()) {
        // If the two nodes are of different size, and the smaller node has the
        // array bit set, collapse!
#if COLLAPSE_ARRAYS_AGGRESSIVELY
        if (SN->getSize() < DN->getSize()) {
          if (SN->isArray()) {
            DN->foldNodeCompletely();
            DN = NH.getNode();
          }
        } else if (DN->isArray()) {
          DN->foldNodeCompletely();
          DN = NH.getNode();
        }
#endif
      }
      if (DN->getSize() < SN->getSize())
        DN->growSize(SN->getSize());

      // Merge the type entries of the two nodes together...
      if (!DN->isNodeCompletelyFolded())
          DN->mergeTypeInfo(SN, NH.getOffset() - SrcNH.getOffset());

    }

    assert(!DN->isDeadNode());

    // Merge the NodeType information.
    DN->mergeNodeFlags(SN->getNodeFlags() & BitsToKeep);

    // Before we start merging outgoing links and updating the scalar map, make
    // sure it is known that this is the representative node for the src node.
    SCNH = DSNodeHandle(DN, NH.getOffset()-SrcNH.getOffset());

    // If the source node contains any globals, make sure they end up in the
    // scalar map with the correct offset.
    if (SN->globals_begin() != SN->globals_end()) {
      // Update the globals in the destination node itself.
      DN->mergeGlobals(*SN);

      // Update the scalar map for the graph we are merging the source node
      // into.
      for (DSNode::globals_iterator I = SN->globals_begin(),
             E = SN->globals_end(); I != E; ++I) {
        const GlobalValue *GV = *I;
        const DSNodeHandle &SrcGNH = Src->getNodeForValue(GV);
        DSNodeHandle &DestGNH = NodeMap[SrcGNH.getNode()];
        assert(DestGNH.getNode()==NH.getNode() &&"Global mapping inconsistent");
        Dest->getNodeForValue(GV).mergeWith(DSNodeHandle(DestGNH.getNode(),
                                      DestGNH.getOffset()+SrcGNH.getOffset()));
      }
      NH.getNode()->mergeGlobals(*SN);
    }
  } else {
    // We cannot handle this case without allocating a temporary node.  Fall
    // back on being simple.
    DSNode *NewDN = new DSNode(*SN, Dest, true /* Null out all links */);
    NewDN->maskNodeTypes(BitsToKeep);

    unsigned NHOffset = NH.getOffset();
    NH.mergeWith(DSNodeHandle(NewDN, SrcNH.getOffset()));

    assert(NH.getNode() &&
           (NH.getOffset() > NHOffset ||
            (NH.getOffset() == 0 && NH.getNode()->isNodeCompletelyFolded())) &&
           "Merging did not adjust the offset!");

    // Before we start merging outgoing links and updating the scalar map, make
    // sure it is known that this is the representative node for the src node.
    SCNH = DSNodeHandle(NH.getNode(), NH.getOffset()-SrcNH.getOffset());

    // If the source node contained any globals, make sure to create entries
    // in the scalar map for them!
    for (DSNode::globals_iterator I = SN->globals_begin(),
           E = SN->globals_end(); I != E; ++I) {
      const GlobalValue *GV = *I;
      const DSNodeHandle &SrcGNH = Src->getNodeForValue(GV);
      DSNodeHandle &DestGNH = NodeMap[SrcGNH.getNode()];
      assert(DestGNH.getNode()==NH.getNode() &&"Global mapping inconsistent");
      assert(SrcGNH.getNode() == SN && "Global mapping inconsistent");
      Dest->getNodeForValue(GV).mergeWith(DSNodeHandle(DestGNH.getNode(),
                                    DestGNH.getOffset()+SrcGNH.getOffset()));
    }
  }

  //  DOUT << "LLVA: mergeWith: " << SN << " becomes " << DN << "\n";

  // Next, recursively merge all outgoing links as necessary.  Note that
  // adding these links can cause the destination node to collapse itself at
  // any time, and the current node may be merged with arbitrary other nodes.
  // For this reason, we must always go through NH.
  DN = 0;
  for (DSNode::const_edge_iterator ii = SN->edge_begin(), ee = SN->edge_end();
       ii != ee; ++ii) {
    const DSNodeHandle &SrcEdge = ii->second;
    if (!SrcEdge.isNull()) {
      // Compute the offset into the current node at which to
      // merge this link.  In the common case, this is a linear
      // relation to the offset in the original node (with
      // wrapping), but if the current node gets collapsed due to
      // recursive merging, we must make sure to merge in all remaining
      // links at offset zero.
      DSNode *CN = SCNH.getNode();
      unsigned MergeOffset = (ii->first+SCNH.getOffset()) % CN->getSize();

      DSNodeHandle Tmp = CN->getLink(MergeOffset);
      if (!Tmp.isNull()) {
        // Perform the recursive merging.  Make sure to create a temporary NH,
        // because the Link can disappear in the process of recursive merging.
        merge(Tmp, SrcEdge);
      } else {
        Tmp.mergeWith(getClonedNH(SrcEdge));
        // Merging this could cause all kinds of recursive things to happen,
        // culminating in the current node being eliminated.  Since this is
        // possible, make sure to reaquire the link from 'CN'.

        unsigned MergeOffset = 0;
        CN = SCNH.getNode();
        MergeOffset = (ii->first + SCNH.getOffset()) % CN->getSize();
        CN->getLink(MergeOffset).mergeWith(Tmp);
      }
    }
  }
}

/// mergeCallSite - Merge the nodes reachable from the specified src call
/// site into the nodes reachable from DestCS.
void ReachabilityCloner::mergeCallSite(DSCallSite &DestCS,
                                       const DSCallSite &SrcCS) {
  merge(DestCS.getRetVal(), SrcCS.getRetVal());
  merge(DestCS.getVAVal(), SrcCS.getVAVal());
  unsigned MinArgs = DestCS.getNumPtrArgs();
  if (SrcCS.getNumPtrArgs() < MinArgs) MinArgs = SrcCS.getNumPtrArgs();

  for (unsigned a = 0; a != MinArgs; ++a)
    merge(DestCS.getPtrArg(a), SrcCS.getPtrArg(a));

  for (unsigned a = MinArgs, e = SrcCS.getNumPtrArgs(); a != e; ++a)
    DestCS.addPtrArg(getClonedNH(SrcCS.getPtrArg(a)));
}

DSCallSite ReachabilityCloner::cloneCallSite(const DSCallSite& SrcCS) {
  std::vector<DSNodeHandle> Args;
  for(unsigned x = 0; x < SrcCS.getNumPtrArgs(); ++x)
    Args.push_back(getClonedNH(SrcCS.getPtrArg(x)));
  if (SrcCS.isDirectCall())
    return DSCallSite(SrcCS.getCallSite(),
                      getClonedNH(SrcCS.getRetVal()),
                      getClonedNH(SrcCS.getVAVal()),
                      SrcCS.getCalleeFunc(),
                      Args);
  else
    return DSCallSite(SrcCS.getCallSite(),
                      getClonedNH(SrcCS.getRetVal()),
                      getClonedNH(SrcCS.getVAVal()),
                      getClonedNH(SrcCS.getCalleeNode()).getNode(),
                      Args);
}

//===----------------------------------------------------------------------===//
// DSCallSite Implementation
//===----------------------------------------------------------------------===//

// Define here to avoid including iOther.h and BasicBlock.h in DSGraph.h
const Function &DSCallSite::getCaller() const {
  return *Site.getInstruction()->getParent()->getParent();
}

void DSCallSite::InitNH(DSNodeHandle &NH, const DSNodeHandle &Src,
                        ReachabilityCloner &RC) {
  NH = RC.getClonedNH(Src);
}

/// FunctionTypeOfCallSite - Helper method to extract the signature of a function
/// that is called a given CallSite
///
const FunctionType *DSCallSite::FunctionTypeOfCallSite(const CallSite & Site) {
  Value *Callee = Site.getCalledValue();

  // Direct call, simple
  if (Function *F = dyn_cast<Function>(Callee))
    return F->getFunctionType();

  // Indirect call, extract the type
  const FunctionType *CalleeFuncType = NULL;

  const PointerType *CalleeType = dyn_cast<PointerType>(Callee->getType());
  if (!CalleeType) {
    assert(0 && "Call through a non-pointer type?");
  } else {
    CalleeFuncType = dyn_cast<FunctionType>(CalleeType->getElementType());
    assert(CalleeFuncType &&
        "Call through pointer to non-function?");
  }

  return CalleeFuncType;
}

/// isVarArg - Determines if the call this represents is to a variable argument
/// function
///
bool DSCallSite::isVarArg() const {
  const FunctionType *FT = FunctionTypeOfCallSite(Site);
  return FT->isVarArg();
}

/// remapLinks - Change all of the Links in the current node according to the
/// specified mapping.
///
void DSNode::remapLinks(DSGraph::NodeMapTy &OldNodeMap) {
  for (LinkMapTy::iterator ii = edge_begin(), ee = edge_end();
       ii != ee; ++ii)
    if (DSNode *N = ii->second.getNode()) {
      DSGraph::NodeMapTy::const_iterator ONMI = OldNodeMap.find(N);
      if (ONMI != OldNodeMap.end()) {
        DSNode *ONMIN = ONMI->second.getNode();
        ii->second.setTo(ONMIN, ii->second.getOffset()+ONMI->second.getOffset());
      }
    }
}

namespace {
  // HackedGraphSCCFinder - This is used to find nodes that have a path from the
  // node to a node cloned by the ReachabilityCloner object contained.  To be
  // extra obnoxious it ignores edges from nodes that are globals, and truncates
  // search at RC marked nodes.  This is designed as an object so that
  // intermediate results can be memoized across invocations of
  // PathExistsToClonedNode.
  struct HackedGraphSCCFinder {
    ReachabilityCloner &RC;
    unsigned CurNodeId;
    std::vector<const DSNode*> SCCStack;
    std::map<const DSNode*, std::pair<unsigned, bool> > NodeInfo;

    HackedGraphSCCFinder(ReachabilityCloner &rc) : RC(rc), CurNodeId(1) {
      // Remove null pointer as a special case.
      NodeInfo[0] = std::make_pair(0, false);
    }

    std::pair<unsigned, bool> &VisitForSCCs(const DSNode *N);

    bool PathExistsToClonedNode(const DSNode *N) {
      return VisitForSCCs(N).second;
    }

    bool PathExistsToClonedNode(const DSCallSite &CS) {
      if (PathExistsToClonedNode(CS.getRetVal().getNode()))
        return true;
      if (CS.isDirectCall() || PathExistsToClonedNode(CS.getCalleeNode()))
        return true;
      if (PathExistsToClonedNode(CS.getVAVal().getNode()))
        return true;
      for (unsigned i = 0, e = CS.getNumPtrArgs(); i != e; ++i)
        if (PathExistsToClonedNode(CS.getPtrArg(i).getNode()))
          return true;
      return false;
    }
  };
}

std::pair<unsigned, bool> &HackedGraphSCCFinder::
VisitForSCCs(const DSNode *N) {
  std::map<const DSNode*, std::pair<unsigned, bool> >::iterator
    NodeInfoIt = NodeInfo.lower_bound(N);
  if (NodeInfoIt != NodeInfo.end() && NodeInfoIt->first == N)
    return NodeInfoIt->second;

  unsigned Min = CurNodeId++;
  unsigned MyId = Min;
  std::pair<unsigned, bool> &ThisNodeInfo =
    NodeInfo.insert(NodeInfoIt,
                    std::make_pair(N, std::make_pair(MyId, false)))->second;

  // Base case: if we find a global, this doesn't reach the cloned graph
  // portion.
  if (N->isGlobalNode()) {
    ThisNodeInfo.second = false;
    return ThisNodeInfo;
  }

  // Base case: if this does reach the cloned graph portion... it does. :)
  if (RC.hasClonedNode(N)) {
    ThisNodeInfo.second = true;
    return ThisNodeInfo;
  }

  SCCStack.push_back(N);

  // Otherwise, check all successors.
  bool AnyDirectSuccessorsReachClonedNodes = false;
  for (DSNode::const_edge_iterator EI = N->edge_begin(), EE = N->edge_end();
       EI != EE; ++EI)
    if (DSNode * Succ = EI->second.getNode()) {
      std::pair<unsigned, bool> &SuccInfo = VisitForSCCs(Succ);
      if (SuccInfo.first < Min) Min = SuccInfo.first;
      AnyDirectSuccessorsReachClonedNodes |= SuccInfo.second;
    }

  if (Min != MyId)
    return ThisNodeInfo;  // Part of a large SCC.  Leave self on stack.

  if (SCCStack.back() == N) {  // Special case single node SCC.
    SCCStack.pop_back();
    ThisNodeInfo.second = AnyDirectSuccessorsReachClonedNodes;
    return ThisNodeInfo;
  }

  // Find out if any direct successors of any node reach cloned nodes.
  if (!AnyDirectSuccessorsReachClonedNodes)
    for (unsigned i = SCCStack.size() - 1; SCCStack[i] != N; --i)
      for (DSNode::const_edge_iterator EI = N->edge_begin(), EE = N->edge_end();
           EI != EE; ++EI)
        if (DSNode * N = EI->second.getNode())
          if (NodeInfo[N].second) {
            AnyDirectSuccessorsReachClonedNodes = true;
            goto OutOfLoop;
          }
OutOfLoop:
  // If any successor reaches a cloned node, mark all nodes in this SCC as
  // reaching the cloned node.
  if (AnyDirectSuccessorsReachClonedNodes)
    while (SCCStack.back() != N) {
      NodeInfo[SCCStack.back()].second = true;
      SCCStack.pop_back();
    }
  SCCStack.pop_back();
  ThisNodeInfo.second = true;
  return ThisNodeInfo;
}

/// markReachableNodes - This method recursively traverses the specified
/// DSNodes, marking any nodes which are reachable.  All reachable nodes it adds
/// to the set, which allows it to only traverse visited nodes once.
///
void DSNode::markReachableNodes(DenseSet<const DSNode*> &ReachableNodes) const {
  if (this == 0) return;
  assert(!isForwarding() && "Cannot mark a forwarded node!");
  if (ReachableNodes.insert(this).second) // Is newly reachable?
    for (DSNode::const_edge_iterator I = edge_begin(), E = edge_end();
         I != E; ++I)
      I->second.getNode()->markReachableNodes(ReachableNodes);
}

void DSCallSite::markReachableNodes(DenseSet<const DSNode*> &Nodes) const {
  getRetVal().getNode()->markReachableNodes(Nodes);
  getVAVal().getNode()->markReachableNodes(Nodes);
  if (isIndirectCall()) getCalleeNode()->markReachableNodes(Nodes);

  for (unsigned i = 0, e = getNumPtrArgs(); i != e; ++i)
    getPtrArg(i).getNode()->markReachableNodes(Nodes);
}

// CanReachAliveNodes - Simple graph walker that recursively traverses the graph
// looking for a node that is marked alive.  If an alive node is found, return
// true, otherwise return false.  If an alive node is reachable, this node is
// marked as alive...
//
static bool CanReachAliveNodes(DSNode *N, DenseSet<const DSNode*> &Alive,
                               DenseSet<const DSNode*> &Visited,
                               bool IgnoreGlobals) {
  if (N == 0) return false;
  assert(N->isForwarding() == 0 && "Cannot mark a forwarded node!");

  // If this is a global node, it will end up in the globals graph anyway, so we
  // don't need to worry about it.
  if (IgnoreGlobals && N->isGlobalNode()) return false;

  // If we know that this node is alive, return so!
  if (Alive.count(N)) return true;

  // Otherwise, we don't think the node is alive yet, check for infinite
  // recursion.
  if (Visited.count(N)) return false;  // Found a cycle
  Visited.insert(N);   // No recursion, insert into Visited...

  for (DSNode::edge_iterator I = N->edge_begin(),E = N->edge_end(); I != E; ++I)
    if (CanReachAliveNodes(I->second.getNode(), Alive, Visited, IgnoreGlobals)) {
      N->markReachableNodes(Alive);
      return true;
    }
  return false;
}

////////////////////////////////////////////////////////////////////////////////
//Base DataStructures impl:
////////////////////////////////////////////////////////////////////////////////

static const Function *getFnForValue(const Value *V) {
  if (const Instruction *I = dyn_cast<Instruction>(V))
    return I->getParent()->getParent();
  else if (const Argument *A = dyn_cast<Argument>(V))
    return A->getParent();
  else if (const BasicBlock *BB = dyn_cast<BasicBlock>(V))
    return BB->getParent();
  return 0;
}

/// deleteValue/copyValue - Interfaces to update the DSGraphs in the program.
/// These correspond to the interfaces defined in the AliasAnalysis class.
void DataStructures::deleteValue(Value *V) {
  if (const Function *F = getFnForValue(V)) {  // Function local value?
    // If this is a function local value, just delete it from the scalar map!
    getDSGraph(*F)->getScalarMap().eraseIfExists(V);
    return;
  }
  
  if (Function *F = dyn_cast<Function>(V)) {
    assert(getDSGraph(*F)->getReturnNodes().size() == 1 &&
           "cannot handle scc's");
    delete DSInfo[F];
    DSInfo.erase(F);
    return;
  }
  
  assert(!isa<GlobalVariable>(V) && "Do not know how to delete GV's yet!");
}

void DataStructures::copyValue(Value *From, Value *To) {
  if (From == To) return;
  if (const Function *F = getFnForValue(From)) {  // Function local value?
    // If this is a function local value, just delete it from the scalar map!
    getDSGraph(*F)->getScalarMap().copyScalarIfExists(From, To);
    return;
  }
  
  if (Function *FromF = dyn_cast<Function>(From)) {
    Function *ToF = cast<Function>(To);
    assert(!DSInfo.count(ToF) && "New Function already exists!");
    DSGraph *NG = new DSGraph(getDSGraph(*FromF), GlobalECs, *TypeSS);
    DSInfo[ToF] = NG;
    assert(NG->getReturnNodes().size() == 1 && "Cannot copy SCC's yet!");
    
    // Change the Function* is the returnnodes map to the ToF.
    DSNodeHandle Ret = NG->retnodes_begin()->second;
    NG->getReturnNodes().clear();
    NG->getReturnNodes()[ToF] = Ret;
    // Change the Function* in the vanodes map to the ToF
    DSNodeHandle VA = NG->vanodes_begin()->second;
    NG->getVANodes().clear();
    NG->getOrCreateVANodeFor(ToF) = VA;
    return;
  }
  
  if (const Function *F = getFnForValue(To)) {
    getDSGraph(*F)->getScalarMap().copyScalarIfExists(From, To);
    return;
  }
  
  errs() << *From;
  errs() << *To;
  assert(0 && "Do not know how to copy this yet!");
  abort();
}

DSGraph* DataStructures::getOrCreateGraph(const Function* F) {
  assert(F && "No function");
  DSGraph *&G = DSInfo[F];
  if (!G) {
    //Clone or Steal the Source Graph
    DSGraph* BaseGraph = GraphSource->getDSGraph(*F);
    if (Clone) {
      G = new DSGraph(BaseGraph, GlobalECs, *TypeSS, DSGraph::DontCloneAuxCallNodes);
    } else {
      G = new DSGraph(GlobalECs, GraphSource->getTargetData(), *TypeSS);
      G->spliceFrom(BaseGraph);
      if (resetAuxCalls) 
        G->getAuxFunctionCalls() = G->getFunctionCalls();
    }
    G->setPrintAuxCalls();
    G->setGlobalsGraph(GlobalsGraph);
    
    // Note that this graph is the graph for ALL of the function in the SCC, not
    // just F.
    for (DSGraph::retnodes_iterator RI = G->retnodes_begin(),
           E = G->retnodes_end(); RI != E; ++RI)
      if (RI->first != F)
        DSInfo[RI->first] = G;
  }
  return G;
}


void DataStructures::formGlobalECs() {
  // Grow the equivalence classes for the globals to include anything that we
  // now know to be aliased.
  svset<const GlobalValue*> ECGlobals;
  buildGlobalECs(ECGlobals);
  if (!ECGlobals.empty()) {
    DEBUG(errs() << "Eliminating " << ECGlobals.size() << " EC Globals!\n");
    for (DSInfoTy::iterator I = DSInfo.begin(),
           E = DSInfo.end(); I != E; ++I)
      eliminateUsesOfECGlobals(*I->second, ECGlobals);
  }
}

/// BuildGlobalECs - Look at all of the nodes in the globals graph.  If any node
/// contains multiple globals, DSA will never, ever, be able to tell the globals
/// apart.  Instead of maintaining this information in all of the graphs
/// throughout the entire program, store only a single global (the "leader") in
/// the graphs, and build equivalence classes for the rest of the globals.
void DataStructures::buildGlobalECs(svset<const GlobalValue*> &ECGlobals) {
  DSScalarMap &SM = GlobalsGraph->getScalarMap();
  EquivalenceClasses<const GlobalValue*> &GlobalECs = SM.getGlobalECs();
  for (DSGraph::node_iterator I = GlobalsGraph->node_begin(), 
         E = GlobalsGraph->node_end();
       I != E; ++I) {
    if (I->numGlobals() <= 1) continue;

    // First, build up the equivalence set for this block of globals.
    DSNode::globals_iterator i = I->globals_begin(); 
    const GlobalValue *First = *i;
    if (GlobalECs.findValue(*i) != GlobalECs.end())
      First = GlobalECs.getLeaderValue(*i);
    if (*i == First) ++i;
    for( ; i != I->globals_end(); ++i) {
      GlobalECs.unionSets(First, *i);
      ECGlobals.insert(*i);
      if (SM.find(*i) != SM.end())
        SM.erase(SM.find(*i));
      else
        errs() << "Global missing in scalar map " << (*i)->getName() << "\n";
    }

    // Next, get the leader element.
    assert(First == GlobalECs.getLeaderValue(First) &&
           "First did not end up being the leader?");

    // Finally, change the global node to only contain the leader.
    I->clearGlobals();
    I->addGlobal(First);
  }

  DEBUG(GlobalsGraph->AssertGraphOK());
}

/// EliminateUsesOfECGlobals - Once we have determined that some globals are in
/// really just equivalent to some other globals, remove the globals from the
/// specified DSGraph (if present), and merge any nodes with their leader nodes.
void DataStructures::eliminateUsesOfECGlobals(DSGraph &G,
                                              const svset<const GlobalValue*> &ECGlobals) {
  DSScalarMap &SM = G.getScalarMap();
  EquivalenceClasses<const GlobalValue*> &GlobalECs = SM.getGlobalECs();

  bool MadeChange = false;
  std::vector<const GlobalValue*> SMGVV(SM.global_begin(), SM.global_end());

  for (std::vector<const GlobalValue*>::iterator GI = SMGVV.begin(),
       E = SMGVV.end(); GI != E; ) {
    const GlobalValue *GV = *GI; ++GI;
    if (!ECGlobals.count(GV)) continue;

    const DSNodeHandle &GVNH = SM[GV];
    assert(!GVNH.isNull() && "Global has null NH!?");

    // Okay, this global is in some equivalence class.  Start by finding the
    // leader of the class.
    const GlobalValue *Leader = GlobalECs.getLeaderValue(GV);

    // If the leader isn't already in the graph, insert it into the node
    // corresponding to GV.
    if (!SM.global_count(Leader)) {
      GVNH.getNode()->addGlobal(Leader);
      SM[Leader] = GVNH;
    } else {
      // Otherwise, the leader is in the graph, make sure the nodes are the
      // merged in the specified graph.
      const DSNodeHandle &LNH = SM[Leader];
      if (LNH.getNode() != GVNH.getNode())
        LNH.mergeWith(GVNH);
    }

    // Next step, remove the global from the DSNode.
    GVNH.getNode()->removeGlobal(GV);

    // Finally, remove the global from the ScalarMap.
    SM.erase(GV);
    MadeChange = true;
  }

  DEBUG(if(MadeChange) G.AssertGraphOK());
}

void DataStructures::init(DataStructures* D, bool clone, bool printAuxCalls, 
                          bool copyGlobalAuxCalls, bool resetAux) {
  assert (!GraphSource && "Already init");
  GraphSource = D;
  Clone = clone;
  resetAuxCalls = resetAux;
  TD = D->TD;
  TypeSS = D->TypeSS;
  callgraph = D->callgraph;
  GlobalECs = D->getGlobalECs();
  GlobalsGraph = new DSGraph(D->getGlobalsGraph(), GlobalECs, *TypeSS,
                             copyGlobalAuxCalls?0:DSGraph::DontCloneAuxCallNodes);
  if (printAuxCalls) GlobalsGraph->setPrintAuxCalls();

  //
  // Tell the other DSA pass if we're stealing its graph.
  //
  if (!clone) D->DSGraphsStolen = true;
}

void DataStructures::init(TargetData* T) {
  assert (!TD && "Already init");
  GraphSource = 0;
  Clone = false;
  TD = T;
  TypeSS = new SuperSet<const Type*>();
  GlobalsGraph = new DSGraph(GlobalECs, *T, *TypeSS);
}

void DataStructures::releaseMemory() {
  //
  // If the DSGraphs were stolen by another pass, free nothing.
  //
  if (DSGraphsStolen) return;

  std::set<DSGraph*> toDelete;
  for (DSInfoTy::iterator I = DSInfo.begin(), E = DSInfo.end(); I != E; ++I) {
    I->second->getReturnNodes().clear();
    toDelete.insert(I->second);
  }
  for (std::set<DSGraph*>::iterator I = toDelete.begin(), E = toDelete.end(); I != E; ++I)
    delete *I;

  // Empty map so next time memory is released, data structures are not
  // re-deleted.
  DSInfo.clear();
  //FIXME:
//  callgraph.clear();
  delete GlobalsGraph;
  GlobalsGraph = 0;
}
