/*
 * Copyright (c) 2015, 2019, Red Hat, Inc. All rights reserved.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"

#include "gc_implementation/shenandoah/shenandoahForwarding.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"
#include "gc_implementation/shenandoah/shenandoahRuntime.hpp"
#include "gc_implementation/shenandoah/c2/shenandoahSupport.hpp"
#include "gc_implementation/shenandoah/c2/shenandoahBarrierSetC2.hpp"
#include "opto/block.hpp"
#include "opto/callnode.hpp"
#include "opto/phaseX.hpp"
#include "opto/rootnode.hpp"
#include "opto/runtime.hpp"
#include "opto/subnode.hpp"

#ifdef _LP64
#define LoadXNode LoadLNode
#else
#define LoadXNode LoadINode
#endif

bool ShenandoahBarrierC2Support::expand(Compile* C, PhaseIterGVN& igvn) {
  if (C->shenandoah_barriers_count() > 0) {
    C->clear_major_progress();
    PhaseIdealLoop ideal_loop(igvn, false, true);
    if (C->failing()) return false;
    PhaseIdealLoop::verify(igvn);
    DEBUG_ONLY(verify_raw_mem(C->root());)
  }
  return true;
}

bool ShenandoahBarrierC2Support::is_gc_state_test(Node* iff, int mask) {
  if (!UseShenandoahGC) {
    return false;
  }
  assert(iff->is_If(), "bad input");
  if (iff->Opcode() != Op_If) {
    return false;
  }
  Node* bol = iff->in(1);
  if (!bol->is_Bool() || bol->as_Bool()->_test._test != BoolTest::ne) {
    return false;
  }
  Node* cmp = bol->in(1);
  if (cmp->Opcode() != Op_CmpI) {
    return false;
  }
  Node* in1 = cmp->in(1);
  Node* in2 = cmp->in(2);
  if (in2->find_int_con(-1) != 0) {
    return false;
  }
  if (in1->Opcode() != Op_AndI) {
    return false;
  }
  in2 = in1->in(2);
  if (in2->find_int_con(-1) != mask) {
    return false;
  }
  in1 = in1->in(1);

  return is_gc_state_load(in1);
}

bool ShenandoahBarrierC2Support::is_heap_stable_test(Node* iff) {
  return is_gc_state_test(iff, ShenandoahHeap::HAS_FORWARDED);
}

bool ShenandoahBarrierC2Support::is_gc_state_load(Node *n) {
  if (!UseShenandoahGC) {
    return false;
  }
  if (n->Opcode() != Op_LoadB && n->Opcode() != Op_LoadUB) {
    return false;
  }
  Node* addp = n->in(MemNode::Address);
  if (!addp->is_AddP()) {
    return false;
  }
  Node* base = addp->in(AddPNode::Address);
  Node* off = addp->in(AddPNode::Offset);
  if (base->Opcode() != Op_ThreadLocal) {
    return false;
  }
  if (off->find_intptr_t_con(-1) != in_bytes(JavaThread::gc_state_offset())) {
    return false;
  }
  return true;
}

bool ShenandoahBarrierC2Support::has_safepoint_between(Node* start, Node* stop, PhaseIdealLoop *phase) {
  assert(phase->is_dominator(stop, start), "bad inputs");
  ResourceMark rm;
  Unique_Node_List wq;
  wq.push(start);
  for (uint next = 0; next < wq.size(); next++) {
    Node *m = wq.at(next);
    if (m == stop) {
      continue;
    }
    if (m->is_SafePoint() && !m->is_CallLeaf()) {
      return true;
    }
    if (m->is_Region()) {
      for (uint i = 1; i < m->req(); i++) {
        wq.push(m->in(i));
      }
    } else {
      wq.push(m->in(0));
    }
  }
  return false;
}

#ifdef ASSERT
bool ShenandoahBarrierC2Support::verify_helper(Node* in, Node_Stack& phis, VectorSet& visited, verify_type t, bool trace, Unique_Node_List& barriers_used) {
  assert(phis.size() == 0, "");

  while (true) {
    if (in->bottom_type() == TypePtr::NULL_PTR) {
      if (trace) {tty->print_cr("NULL");}
    } else if (!in->bottom_type()->make_ptr()->make_oopptr()) {
      if (trace) {tty->print_cr("Non oop");}
    } else if (in->bottom_type()->make_ptr()->make_oopptr() == TypeInstPtr::MIRROR) {
      if (trace) {tty->print_cr("Java mirror");}
    } else {
      if (in->is_ConstraintCast() || in->Opcode() == Op_CheckCastPP) {
        in = in->in(1);
        continue;
      } else if (in->is_AddP()) {
        assert(!in->in(AddPNode::Address)->is_top(), "no raw memory access");
        in = in->in(AddPNode::Address);
        continue;
      } else if (in->is_Con()) {
        if (trace) {
          tty->print("Found constant");
          in->dump();
        }
      } else if (in->Opcode() == Op_Parm) {
        if (trace) {
          tty->print("Found argument");
        }
      } else if (in->Opcode() == Op_CreateEx) {
        if (trace) {
          tty->print("Found create-exception");
        }
      } else if (in->Opcode() == Op_LoadP && in->adr_type() == TypeRawPtr::BOTTOM) {
        if (trace) {
          tty->print("Found raw LoadP (OSR argument?)");
        }
      } else if (in->Opcode() == Op_ShenandoahLoadReferenceBarrier) {
        if (t == ShenandoahOopStore) {
          uint i = 0;
          for (; i < phis.size(); i++) {
            Node* n = phis.node_at(i);
          }
          if (i == phis.size()) {
            return false;
          }
        }
        barriers_used.push(in);
        if (trace) {tty->print("Found barrier"); in->dump();}
      } else if (in->is_Proj() && in->in(0)->is_Allocate()) {
        if (trace) {
          tty->print("Found alloc");
          in->in(0)->dump();
        }
      } else if (in->is_Proj() && (in->in(0)->Opcode() == Op_CallStaticJava || in->in(0)->Opcode() == Op_CallDynamicJava)) {
        if (trace) {
          tty->print("Found Java call");
        }
      } else if (in->is_Phi()) {
        if (!visited.test_set(in->_idx)) {
          if (trace) {tty->print("Pushed phi:"); in->dump();}
          phis.push(in, 2);
          in = in->in(1);
          continue;
        }
        if (trace) {tty->print("Already seen phi:"); in->dump();}
      } else if (in->Opcode() == Op_CMoveP || in->Opcode() == Op_CMoveN) {
        if (!visited.test_set(in->_idx)) {
          if (trace) {tty->print("Pushed cmovep:"); in->dump();}
          phis.push(in, CMoveNode::IfTrue);
          in = in->in(CMoveNode::IfFalse);
          continue;
        }
        if (trace) {tty->print("Already seen cmovep:"); in->dump();}
      } else if (in->Opcode() == Op_EncodeP || in->Opcode() == Op_DecodeN) {
        in = in->in(1);
        continue;
      } else {
        return false;
      }
    }
    bool cont = false;
    while (phis.is_nonempty()) {
      uint idx = phis.index();
      Node* phi = phis.node();
      if (idx >= phi->req()) {
        if (trace) {tty->print("Popped phi:"); phi->dump();}
        phis.pop();
        continue;
      }
      if (trace) {tty->print("Next entry(%d) for phi:", idx); phi->dump();}
      in = phi->in(idx);
      phis.set_index(idx+1);
      cont = true;
      break;
    }
    if (!cont) {
      break;
    }
  }
  return true;
}

void ShenandoahBarrierC2Support::report_verify_failure(const char* msg, Node* n1, Node* n2) {
  if (n1 != NULL) {
    n1->dump(+10);
  }
  if (n2 != NULL) {
    n2->dump(+10);
  }
  fatal(err_msg("%s", msg));
}

static const char* call_name(CallNode* call) {
  if (call->is_CallRuntime()) {
    return call->as_CallRuntime()->_name;
  }
  if (call->is_CallStaticJava()) {
    return call->as_CallStaticJava()->_name;
  }
  return NULL;
}

void ShenandoahBarrierC2Support::verify(RootNode* root) {
  ResourceMark rm;
  Unique_Node_List wq;
  GrowableArray<Node*> barriers;
  Unique_Node_List barriers_used;
  Node_Stack phis(0);
  VectorSet visited(Thread::current()->resource_area());
  const bool trace = true;
  const bool verify_no_useless_barrier = false;

  wq.push(root);
  for (uint next = 0; next < wq.size(); next++) {
    Node *n = wq.at(next);
    if (n->is_Load()) {
      const bool trace = false;
      if (trace) {tty->print("Verifying"); n->dump();}
      if (n->Opcode() == Op_LoadRange || n->Opcode() == Op_LoadKlass || n->Opcode() == Op_LoadNKlass) {
        if (trace) {tty->print_cr("Load range/klass");}
      } else {
        const TypePtr* adr_type = n->as_Load()->adr_type();

        if (adr_type->isa_oopptr() && adr_type->is_oopptr()->offset() == oopDesc::mark_offset_in_bytes()) {
          if (trace) {tty->print_cr("Mark load");}
        } else if (adr_type->isa_instptr() &&
                   adr_type->is_instptr()->klass()->is_subtype_of(Compile::current()->env()->Reference_klass()) &&
                   adr_type->is_instptr()->offset() == java_lang_ref_Reference::referent_offset) {
          if (trace) {tty->print_cr("Reference.get()");}
        } else if (!verify_helper(n->in(MemNode::Address), phis, visited, ShenandoahLoad, trace, barriers_used)) {
          report_verify_failure("Shenandoah verification: Load should have barriers", n);
        }
      }
    } else if (n->is_Store()) {
      const bool trace = false;

      if (trace) {tty->print("Verifying"); n->dump();}
      if (n->in(MemNode::ValueIn)->bottom_type()->make_oopptr()) {
        Node* adr = n->in(MemNode::Address);
        bool verify = true;

        if (adr->is_AddP() && adr->in(AddPNode::Base)->is_top()) {
          adr = adr->in(AddPNode::Address);
          if (adr->is_AddP()) {
            assert(adr->in(AddPNode::Base)->is_top(), "");
            adr = adr->in(AddPNode::Address);
            if (adr->Opcode() == Op_LoadP &&
                adr->in(MemNode::Address)->in(AddPNode::Base)->is_top() &&
                adr->in(MemNode::Address)->in(AddPNode::Address)->Opcode() == Op_ThreadLocal &&
                adr->in(MemNode::Address)->in(AddPNode::Offset)->find_intptr_t_con(-1) == in_bytes(JavaThread::satb_mark_queue_offset()) + in_bytes(PtrQueue::byte_offset_of_buf())) {
              if (trace) {tty->print_cr("SATB prebarrier");}
              verify = false;
            }
          }
        }

        if (verify && !verify_helper(n->in(MemNode::ValueIn), phis, visited, ShenandoahValue, trace, barriers_used)) {
          report_verify_failure("Shenandoah verification: Store should have barriers", n);
        }
      }
      if (!verify_helper(n->in(MemNode::Address), phis, visited, ShenandoahStore, trace, barriers_used)) {
        report_verify_failure("Shenandoah verification: Store (address) should have barriers", n);
      }
    } else if (n->Opcode() == Op_CmpP) {
      const bool trace = false;

      Node* in1 = n->in(1);
      Node* in2 = n->in(2);
      if (in1->bottom_type()->isa_oopptr()) {
        if (trace) {tty->print("Verifying"); n->dump();}

        bool mark_inputs = false;
        if (in1->bottom_type() == TypePtr::NULL_PTR || in2->bottom_type() == TypePtr::NULL_PTR ||
            (in1->is_Con() || in2->is_Con())) {
          if (trace) {tty->print_cr("Comparison against a constant");}
          mark_inputs = true;
        } else if ((in1->is_CheckCastPP() && in1->in(1)->is_Proj() && in1->in(1)->in(0)->is_Allocate()) ||
                   (in2->is_CheckCastPP() && in2->in(1)->is_Proj() && in2->in(1)->in(0)->is_Allocate())) {
          if (trace) {tty->print_cr("Comparison with newly alloc'ed object");}
          mark_inputs = true;
        } else {
          assert(in2->bottom_type()->isa_oopptr(), "");

          if (!verify_helper(in1, phis, visited, ShenandoahStore, trace, barriers_used) ||
              !verify_helper(in2, phis, visited, ShenandoahStore, trace, barriers_used)) {
            report_verify_failure("Shenandoah verification: Cmp should have barriers", n);
          }
        }
        if (verify_no_useless_barrier &&
            mark_inputs &&
            (!verify_helper(in1, phis, visited, ShenandoahValue, trace, barriers_used) ||
             !verify_helper(in2, phis, visited, ShenandoahValue, trace, barriers_used))) {
          phis.clear();
          visited.Reset();
        }
      }
    } else if (n->is_LoadStore()) {
      if (n->in(MemNode::ValueIn)->bottom_type()->make_ptr() &&
          !verify_helper(n->in(MemNode::ValueIn), phis, visited, ShenandoahValue, trace, barriers_used)) {
        report_verify_failure("Shenandoah verification: LoadStore (value) should have barriers", n);
      }

      if (n->in(MemNode::Address)->bottom_type()->make_oopptr() && !verify_helper(n->in(MemNode::Address), phis, visited, ShenandoahStore, trace, barriers_used)) {
        report_verify_failure("Shenandoah verification: LoadStore (address) should have barriers", n);
      }
    } else if (n->Opcode() == Op_CallLeafNoFP || n->Opcode() == Op_CallLeaf) {
      CallNode* call = n->as_Call();

      static struct {
        const char* name;
        struct {
          int pos;
          verify_type t;
        } args[6];
      } calls[] = {
        "aescrypt_encryptBlock",
        { { TypeFunc::Parms, ShenandoahLoad },   { TypeFunc::Parms+1, ShenandoahStore },  { TypeFunc::Parms+2, ShenandoahLoad },
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "aescrypt_decryptBlock",
        { { TypeFunc::Parms, ShenandoahLoad },   { TypeFunc::Parms+1, ShenandoahStore },  { TypeFunc::Parms+2, ShenandoahLoad },
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "multiplyToLen",
        { { TypeFunc::Parms, ShenandoahLoad },   { TypeFunc::Parms+2, ShenandoahLoad },   { TypeFunc::Parms+4, ShenandoahStore },
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "squareToLen",
        { { TypeFunc::Parms, ShenandoahLoad },   { TypeFunc::Parms+2, ShenandoahLoad },   { -1,  ShenandoahNone},
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "montgomery_multiply",
        { { TypeFunc::Parms, ShenandoahLoad },   { TypeFunc::Parms+1, ShenandoahLoad },   { TypeFunc::Parms+2, ShenandoahLoad },
          { TypeFunc::Parms+6, ShenandoahStore }, { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "montgomery_square",
        { { TypeFunc::Parms, ShenandoahLoad },   { TypeFunc::Parms+1, ShenandoahLoad },   { TypeFunc::Parms+5, ShenandoahStore },
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "mulAdd",
        { { TypeFunc::Parms, ShenandoahStore },  { TypeFunc::Parms+1, ShenandoahLoad },   { -1,  ShenandoahNone},
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "vectorizedMismatch",
        { { TypeFunc::Parms, ShenandoahLoad },   { TypeFunc::Parms+1, ShenandoahLoad },   { -1,  ShenandoahNone},
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "updateBytesCRC32",
        { { TypeFunc::Parms+1, ShenandoahLoad }, { -1,  ShenandoahNone},                  { -1,  ShenandoahNone},
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "updateBytesAdler32",
        { { TypeFunc::Parms+1, ShenandoahLoad }, { -1,  ShenandoahNone},                  { -1,  ShenandoahNone},
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "updateBytesCRC32C",
        { { TypeFunc::Parms+1, ShenandoahLoad }, { TypeFunc::Parms+3, ShenandoahLoad},    { -1,  ShenandoahNone},
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "counterMode_AESCrypt",
        { { TypeFunc::Parms, ShenandoahLoad },   { TypeFunc::Parms+1, ShenandoahStore },  { TypeFunc::Parms+2, ShenandoahLoad },
          { TypeFunc::Parms+3, ShenandoahStore }, { TypeFunc::Parms+5, ShenandoahStore }, { TypeFunc::Parms+6, ShenandoahStore } },
        "cipherBlockChaining_encryptAESCrypt",
        { { TypeFunc::Parms, ShenandoahLoad },   { TypeFunc::Parms+1, ShenandoahStore },  { TypeFunc::Parms+2, ShenandoahLoad },
          { TypeFunc::Parms+3, ShenandoahLoad },  { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "cipherBlockChaining_decryptAESCrypt",
        { { TypeFunc::Parms, ShenandoahLoad },   { TypeFunc::Parms+1, ShenandoahStore },  { TypeFunc::Parms+2, ShenandoahLoad },
          { TypeFunc::Parms+3, ShenandoahLoad },  { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "shenandoah_clone_barrier",
        { { TypeFunc::Parms, ShenandoahLoad },   { -1,  ShenandoahNone},                  { -1,  ShenandoahNone},
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "ghash_processBlocks",
        { { TypeFunc::Parms, ShenandoahStore },  { TypeFunc::Parms+1, ShenandoahLoad },   { TypeFunc::Parms+2, ShenandoahLoad },
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "sha1_implCompress",
        { { TypeFunc::Parms, ShenandoahLoad },  { TypeFunc::Parms+1, ShenandoahStore },   { -1, ShenandoahNone },
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "sha256_implCompress",
        { { TypeFunc::Parms, ShenandoahLoad },  { TypeFunc::Parms+1, ShenandoahStore },   { -1, ShenandoahNone },
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "sha512_implCompress",
        { { TypeFunc::Parms, ShenandoahLoad },  { TypeFunc::Parms+1, ShenandoahStore },   { -1, ShenandoahNone },
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "sha1_implCompressMB",
        { { TypeFunc::Parms, ShenandoahLoad },  { TypeFunc::Parms+1, ShenandoahStore },   { -1, ShenandoahNone },
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "sha256_implCompressMB",
        { { TypeFunc::Parms, ShenandoahLoad },  { TypeFunc::Parms+1, ShenandoahStore },   { -1, ShenandoahNone },
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "sha512_implCompressMB",
        { { TypeFunc::Parms, ShenandoahLoad },  { TypeFunc::Parms+1, ShenandoahStore },   { -1, ShenandoahNone },
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "encodeBlock",
        { { TypeFunc::Parms, ShenandoahLoad },  { TypeFunc::Parms+3, ShenandoahStore },   { -1, ShenandoahNone },
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
      };

      if (call->is_CallRuntime() && call->as_CallRuntime()->is_call_to_arraycopystub()) {
        Node* dest = NULL;
        const TypeTuple* args = n->as_Call()->_tf->domain();
        for (uint i = TypeFunc::Parms, j = 0; i < args->cnt(); i++) {
          if (args->field_at(i)->isa_ptr()) {
            j++;
            if (j == 2) {
              dest = n->in(i);
              break;
            }
          }
        }
        if (!verify_helper(n->in(TypeFunc::Parms), phis, visited, ShenandoahLoad, trace, barriers_used) ||
            !verify_helper(dest, phis, visited, ShenandoahStore, trace, barriers_used)) {
          report_verify_failure("Shenandoah verification: ArrayCopy should have barriers", n);
        }
      } else if (strlen(call_name(call)) > 5 &&
                 !strcmp(call_name(call) + strlen(call_name(call)) - 5, "_fill")) {
        if (!verify_helper(n->in(TypeFunc::Parms), phis, visited, ShenandoahStore, trace, barriers_used)) {
          report_verify_failure("Shenandoah verification: _fill should have barriers", n);
        }
      } else if (!strcmp(call_name(call), "g1_wb_pre")) {
        // skip
      } else {
        const int calls_len = sizeof(calls) / sizeof(calls[0]);
        int i = 0;
        for (; i < calls_len; i++) {
          if (!strcmp(calls[i].name, call_name(call))) {
            break;
          }
        }
        if (i != calls_len) {
          const uint args_len = sizeof(calls[0].args) / sizeof(calls[0].args[0]);
          for (uint j = 0; j < args_len; j++) {
            int pos = calls[i].args[j].pos;
            if (pos == -1) {
              break;
            }
            if (!verify_helper(call->in(pos), phis, visited, calls[i].args[j].t, trace, barriers_used)) {
              report_verify_failure("Shenandoah verification: intrinsic calls should have barriers", n);
            }
          }
          for (uint j = TypeFunc::Parms; j < call->req(); j++) {
            if (call->in(j)->bottom_type()->make_ptr() &&
                call->in(j)->bottom_type()->make_ptr()->isa_oopptr()) {
              uint k = 0;
              for (; k < args_len && calls[i].args[k].pos != (int)j; k++);
              if (k == args_len) {
                fatal(err_msg("arg %d for call %s not covered", j, call_name(call)));
              }
            }
          }
        } else {
          for (uint j = TypeFunc::Parms; j < call->req(); j++) {
            if (call->in(j)->bottom_type()->make_ptr() &&
                call->in(j)->bottom_type()->make_ptr()->isa_oopptr()) {
              fatal(err_msg("%s not covered", call_name(call)));
            }
          }
        }
      }
    } else if (n->Opcode() == Op_ShenandoahLoadReferenceBarrier) {
      // skip
    } else if (n->is_AddP()
               || n->is_Phi()
               || n->is_ConstraintCast()
               || n->Opcode() == Op_CheckCastPP
               || n->Opcode() == Op_Return
               || n->Opcode() == Op_CMoveP
               || n->Opcode() == Op_CMoveN
               || n->Opcode() == Op_Rethrow
               || n->is_MemBar()
               || n->Opcode() == Op_Conv2B
               || n->Opcode() == Op_SafePoint
               || n->is_CallJava()
               || n->Opcode() == Op_Unlock
               || n->Opcode() == Op_EncodeP
               || n->Opcode() == Op_DecodeN) {
      // nothing to do
    } else {
      static struct {
        int opcode;
        struct {
          int pos;
          verify_type t;
        } inputs[2];
      } others[] = {
        Op_FastLock,
        { { 1, ShenandoahLoad },                  { -1, ShenandoahNone} },
        Op_Lock,
        { { TypeFunc::Parms, ShenandoahLoad },    { -1, ShenandoahNone} },
        Op_AryEq,
        { { 2, ShenandoahLoad },                  { 3, ShenandoahLoad } },
        Op_StrIndexOf,
        { { 2, ShenandoahLoad },                  { 4, ShenandoahLoad } },
        Op_StrComp,
        { { 2, ShenandoahLoad },                  { 4, ShenandoahLoad } },
        Op_StrEquals,
        { { 2, ShenandoahLoad },                  { 3, ShenandoahLoad } },
        Op_EncodeISOArray,
        { { 2, ShenandoahLoad },                  { 3, ShenandoahStore } },
        Op_CastP2X,
        { { 1, ShenandoahLoad },                  { -1, ShenandoahNone} },
        Op_ClearArray,
        { { 3, ShenandoahStore },                 { -1, ShenandoahNone} },
      };

      const int others_len = sizeof(others) / sizeof(others[0]);
      int i = 0;
      for (; i < others_len; i++) {
        if (others[i].opcode == n->Opcode()) {
          break;
        }
      }
      uint stop = n->is_Call() ? n->as_Call()->tf()->domain()->cnt() : n->req();
      if (i != others_len) {
        const uint inputs_len = sizeof(others[0].inputs) / sizeof(others[0].inputs[0]);
        for (uint j = 0; j < inputs_len; j++) {
          int pos = others[i].inputs[j].pos;
          if (pos == -1) {
            break;
          }
          if (!verify_helper(n->in(pos), phis, visited, others[i].inputs[j].t, trace, barriers_used)) {
            report_verify_failure("Shenandoah verification: intrinsic calls should have barriers", n);
          }
        }
        for (uint j = 1; j < stop; j++) {
          if (n->in(j) != NULL && n->in(j)->bottom_type()->make_ptr() &&
              n->in(j)->bottom_type()->make_ptr()->make_oopptr()) {
            uint k = 0;
            for (; k < inputs_len && others[i].inputs[k].pos != (int)j; k++);
            if (k == inputs_len) {
              fatal(err_msg("arg %d for node %s not covered", j, n->Name()));
            }
          }
        }
      } else {
        for (uint j = 1; j < stop; j++) {
          if (n->in(j) != NULL && n->in(j)->bottom_type()->make_ptr() &&
              n->in(j)->bottom_type()->make_ptr()->make_oopptr()) {
            fatal(err_msg("%s not covered", n->Name()));
          }
        }
      }
    }

    if (n->is_SafePoint()) {
      SafePointNode* sfpt = n->as_SafePoint();
      if (verify_no_useless_barrier && sfpt->jvms() != NULL) {
        for (uint i = sfpt->jvms()->scloff(); i < sfpt->jvms()->endoff(); i++) {
          if (!verify_helper(sfpt->in(i), phis, visited, ShenandoahLoad, trace, barriers_used)) {
            phis.clear();
            visited.Reset();
          }
        }
      }
    }
  }

  if (verify_no_useless_barrier) {
    for (int i = 0; i < barriers.length(); i++) {
      Node* n = barriers.at(i);
      if (!barriers_used.member(n)) {
        tty->print("XXX useless barrier"); n->dump(-2);
        ShouldNotReachHere();
      }
    }
  }
}
#endif

bool ShenandoahBarrierC2Support::is_dominator_same_ctrl(Node* c, Node* d, Node* n, PhaseIdealLoop* phase) {
  // That both nodes have the same control is not sufficient to prove
  // domination, verify that there's no path from d to n
  ResourceMark rm;
  Unique_Node_List wq;
  wq.push(d);
  for (uint next = 0; next < wq.size(); next++) {
    Node *m = wq.at(next);
    if (m == n) {
      return false;
    }
    if (m->is_Phi() && m->in(0)->is_Loop()) {
      assert(phase->ctrl_or_self(m->in(LoopNode::EntryControl)) != c, "following loop entry should lead to new control");
    } else {
      if (m->is_Store() || m->is_LoadStore()) {
        // Take anti-dependencies into account
        Node* mem = m->in(MemNode::Memory);
        for (DUIterator_Fast imax, i = mem->fast_outs(imax); i < imax; i++) {
          Node* u = mem->fast_out(i);
          if (u->is_Load() && phase->C->can_alias(m->adr_type(), phase->C->get_alias_index(u->adr_type())) &&
              phase->ctrl_or_self(u) == c) {
            wq.push(u);
          }
        }
      }
      for (uint i = 0; i < m->req(); i++) {
        if (m->in(i) != NULL && phase->ctrl_or_self(m->in(i)) == c) {
          wq.push(m->in(i));
        }
      }
    }
  }
  return true;
}

bool ShenandoahBarrierC2Support::is_dominator(Node* d_c, Node* n_c, Node* d, Node* n, PhaseIdealLoop* phase) {
  if (d_c != n_c) {
    return phase->is_dominator(d_c, n_c);
  }
  return is_dominator_same_ctrl(d_c, d, n, phase);
}

Node* next_mem(Node* mem, int alias) {
  Node* res = NULL;
  if (mem->is_Proj()) {
    res = mem->in(0);
  } else if (mem->is_SafePoint() || mem->is_MemBar()) {
    res = mem->in(TypeFunc::Memory);
  } else if (mem->is_Phi()) {
    res = mem->in(1);
  } else if (mem->is_MergeMem()) {
    res = mem->as_MergeMem()->memory_at(alias);
  } else if (mem->is_Store() || mem->is_LoadStore() || mem->is_ClearArray()) {
    assert(alias = Compile::AliasIdxRaw, "following raw memory can't lead to a barrier");
    res = mem->in(MemNode::Memory);
  } else {
#ifdef ASSERT
    mem->dump();
#endif
    ShouldNotReachHere();
  }
  return res;
}

Node* ShenandoahBarrierC2Support::no_branches(Node* c, Node* dom, bool allow_one_proj, PhaseIdealLoop* phase) {
  Node* iffproj = NULL;
  while (c != dom) {
    Node* next = phase->idom(c);
    assert(next->unique_ctrl_out() == c || c->is_Proj() || c->is_Region(), "multiple control flow out but no proj or region?");
    if (c->is_Region()) {
      ResourceMark rm;
      Unique_Node_List wq;
      wq.push(c);
      for (uint i = 0; i < wq.size(); i++) {
        Node *n = wq.at(i);
        if (n == next) {
          continue;
        }
        if (n->is_Region()) {
          for (uint j = 1; j < n->req(); j++) {
            wq.push(n->in(j));
          }
        } else {
          wq.push(n->in(0));
        }
      }
      for (uint i = 0; i < wq.size(); i++) {
        Node *n = wq.at(i);
        assert(n->is_CFG(), "");
        if (n->is_Multi()) {
          for (DUIterator_Fast jmax, j = n->fast_outs(jmax); j < jmax; j++) {
            Node* u = n->fast_out(j);
            if (u->is_CFG()) {
              if (!wq.member(u) && !u->as_Proj()->is_uncommon_trap_proj(Deoptimization::Reason_none)) {
                return NodeSentinel;
              }
            }
          }
        }
      }
    } else  if (c->is_Proj()) {
      if (c->is_IfProj()) {
        if (c->as_Proj()->is_uncommon_trap_if_pattern(Deoptimization::Reason_none) != NULL) {
          // continue;
        } else {
          if (!allow_one_proj) {
            return NodeSentinel;
          }
          if (iffproj == NULL) {
            iffproj = c;
          } else {
            return NodeSentinel;
          }
        }
      } else if (c->Opcode() == Op_JumpProj) {
        return NodeSentinel; // unsupported
      } else if (c->Opcode() == Op_CatchProj) {
        return NodeSentinel; // unsupported
      } else if (c->Opcode() == Op_CProj && next->Opcode() == Op_NeverBranch) {
        return NodeSentinel; // unsupported
      } else {
        assert(next->unique_ctrl_out() == c, "unsupported branch pattern");
      }
    }
    c = next;
  }
  return iffproj;
}

Node* ShenandoahBarrierC2Support::dom_mem(Node* mem, Node* ctrl, int alias, Node*& mem_ctrl, PhaseIdealLoop* phase) {
  ResourceMark rm;
  VectorSet wq(Thread::current()->resource_area());
  wq.set(mem->_idx);
  mem_ctrl = phase->ctrl_or_self(mem);
  while (!phase->is_dominator(mem_ctrl, ctrl) || mem_ctrl == ctrl) {
    mem = next_mem(mem, alias);
    if (wq.test_set(mem->_idx)) {
      return NULL;
    }
    mem_ctrl = phase->ctrl_or_self(mem);
  }
  if (mem->is_MergeMem()) {
    mem = mem->as_MergeMem()->memory_at(alias);
    mem_ctrl = phase->ctrl_or_self(mem);
  }
  return mem;
}

Node* ShenandoahBarrierC2Support::find_bottom_mem(Node* ctrl, PhaseIdealLoop* phase) {
  Node* mem = NULL;
  Node* c = ctrl;
  do {
    if (c->is_Region()) {
      Node* phi_bottom = NULL;
      for (DUIterator_Fast imax, i = c->fast_outs(imax); i < imax && mem == NULL; i++) {
        Node* u = c->fast_out(i);
        if (u->is_Phi() && u->bottom_type() == Type::MEMORY) {
          if (u->adr_type() == TypePtr::BOTTOM) {
            mem = u;
          }
        }
      }
    } else {
      if (c->is_Call() && c->as_Call()->adr_type() != NULL) {
        CallProjections projs;
        c->as_Call()->extract_projections(&projs, true, false);
        if (projs.fallthrough_memproj != NULL) {
          if (projs.fallthrough_memproj->adr_type() == TypePtr::BOTTOM) {
            if (projs.catchall_memproj == NULL) {
              mem = projs.fallthrough_memproj;
            } else {
              if (phase->is_dominator(projs.fallthrough_catchproj, ctrl)) {
                mem = projs.fallthrough_memproj;
              } else {
                assert(phase->is_dominator(projs.catchall_catchproj, ctrl), "one proj must dominate barrier");
                mem = projs.catchall_memproj;
              }
            }
          }
        } else {
          Node* proj = c->as_Call()->proj_out(TypeFunc::Memory);
          if (proj != NULL &&
              proj->adr_type() == TypePtr::BOTTOM) {
            mem = proj;
          }
        }
      } else {
        for (DUIterator_Fast imax, i = c->fast_outs(imax); i < imax; i++) {
          Node* u = c->fast_out(i);
          if (u->is_Proj() &&
              u->bottom_type() == Type::MEMORY &&
              u->adr_type() == TypePtr::BOTTOM) {
              assert(c->is_SafePoint() || c->is_MemBar() || c->is_Start(), "");
              assert(mem == NULL, "only one proj");
              mem = u;
          }
        }
        assert(!c->is_Call() || c->as_Call()->adr_type() != NULL || mem == NULL, "no mem projection expected");
      }
    }
    c = phase->idom(c);
  } while (mem == NULL);
  return mem;
}

void ShenandoahBarrierC2Support::follow_barrier_uses(Node* n, Node* ctrl, Unique_Node_List& uses, PhaseIdealLoop* phase) {
  for (DUIterator_Fast imax, i = n->fast_outs(imax); i < imax; i++) {
    Node* u = n->fast_out(i);
    if (!u->is_CFG() && phase->get_ctrl(u) == ctrl && (!u->is_Phi() || !u->in(0)->is_Loop() || u->in(LoopNode::LoopBackControl) != n)) {
      uses.push(u);
    }
  }
}

void ShenandoahBarrierC2Support::test_gc_state(Node*& ctrl, Node* raw_mem, Node*& test_fail_ctrl,
                                               PhaseIdealLoop* phase, int flags) {
  PhaseIterGVN& igvn = phase->igvn();
  Node* old_ctrl = ctrl;

  Node* thread          = new (phase->C) ThreadLocalNode();
  Node* gc_state_offset = igvn.MakeConX(in_bytes(JavaThread::gc_state_offset()));
  Node* gc_state_addr   = new (phase->C) AddPNode(phase->C->top(), thread, gc_state_offset);
  Node* gc_state        = new (phase->C) LoadBNode(old_ctrl, raw_mem, gc_state_addr,
                                                   DEBUG_ONLY(phase->C->get_adr_type(Compile::AliasIdxRaw)) NOT_DEBUG(NULL),
                                                   TypeInt::BYTE, MemNode::unordered);

  Node* gc_state_and    = new (phase->C) AndINode(gc_state, igvn.intcon(flags));
  Node* gc_state_cmp    = new (phase->C) CmpINode(gc_state_and, igvn.zerocon(T_INT));
  Node* gc_state_bool   = new (phase->C) BoolNode(gc_state_cmp, BoolTest::ne);

  IfNode* gc_state_iff  = new (phase->C) IfNode(old_ctrl, gc_state_bool, PROB_UNLIKELY(0.999), COUNT_UNKNOWN);
  ctrl                  = new (phase->C) IfTrueNode(gc_state_iff);
  test_fail_ctrl        = new (phase->C) IfFalseNode(gc_state_iff);

  IdealLoopTree* loop = phase->get_loop(old_ctrl);
  phase->register_control(gc_state_iff,   loop, old_ctrl);
  phase->register_control(ctrl,           loop, gc_state_iff);
  phase->register_control(test_fail_ctrl, loop, gc_state_iff);

  phase->register_new_node(thread,        old_ctrl);
  phase->register_new_node(gc_state_addr, old_ctrl);
  phase->register_new_node(gc_state,      old_ctrl);
  phase->register_new_node(gc_state_and,  old_ctrl);
  phase->register_new_node(gc_state_cmp,  old_ctrl);
  phase->register_new_node(gc_state_bool, old_ctrl);

  phase->set_ctrl(gc_state_offset, phase->C->root());
  assert(is_gc_state_test(gc_state_iff, flags), "Should match the shape");
}

void ShenandoahBarrierC2Support::test_null(Node*& ctrl, Node* val, Node*& null_ctrl, PhaseIdealLoop* phase) {
  Node* old_ctrl = ctrl;
  PhaseIterGVN& igvn = phase->igvn();

  const Type* val_t = igvn.type(val);
  if (val_t->meet(TypePtr::NULL_PTR) == val_t) {
    Node* null_cmp   = new (phase->C) CmpPNode(val, igvn.zerocon(T_OBJECT));
    Node* null_test  = new (phase->C) BoolNode(null_cmp, BoolTest::ne);

    IfNode* null_iff = new (phase->C) IfNode(old_ctrl, null_test, PROB_LIKELY(0.999), COUNT_UNKNOWN);
    ctrl             = new (phase->C) IfTrueNode(null_iff);
    null_ctrl        = new (phase->C) IfFalseNode(null_iff);

    IdealLoopTree* loop = phase->get_loop(old_ctrl);
    phase->register_control(null_iff,  loop, old_ctrl);
    phase->register_control(ctrl,      loop, null_iff);
    phase->register_control(null_ctrl, loop, null_iff);

    phase->register_new_node(null_cmp,  old_ctrl);
    phase->register_new_node(null_test, old_ctrl);
  }
}

Node* ShenandoahBarrierC2Support::clone_null_check(Node*& c, Node* val, Node* unc_ctrl, PhaseIdealLoop* phase) {
  IdealLoopTree *loop = phase->get_loop(c);
  Node* iff = unc_ctrl->in(0);
  assert(iff->is_If(), "broken");
  Node* new_iff = iff->clone();
  new_iff->set_req(0, c);
  phase->register_control(new_iff, loop, c);
  Node* iffalse = new (phase->C) IfFalseNode(new_iff->as_If());
  phase->register_control(iffalse, loop, new_iff);
  Node* iftrue = new (phase->C) IfTrueNode(new_iff->as_If());
  phase->register_control(iftrue, loop, new_iff);
  c = iftrue;
  const Type *t = phase->igvn().type(val);
  assert(val->Opcode() == Op_CastPP, "expect cast to non null here");
  Node* uncasted_val = val->in(1);
  val = new (phase->C) CastPPNode(uncasted_val, t);
  val->init_req(0, c);
  phase->register_new_node(val, c);
  return val;
}

void ShenandoahBarrierC2Support::fix_null_check(Node* unc, Node* unc_ctrl, Node* new_unc_ctrl,
                                                Unique_Node_List& uses, PhaseIdealLoop* phase) {
  IfNode* iff = unc_ctrl->in(0)->as_If();
  Node* proj = iff->proj_out(0);
  assert(proj != unc_ctrl, "bad projection");
  Node* use = proj->unique_ctrl_out();

  assert(use == unc || use->is_Region(), "what else?");

  uses.clear();
  if (use == unc) {
    phase->set_idom(use, new_unc_ctrl, phase->dom_depth(use));
    for (uint i = 1; i < unc->req(); i++) {
      Node* n = unc->in(i);
      if (phase->has_ctrl(n) && phase->get_ctrl(n) == proj) {
        uses.push(n);
      }
    }
  } else {
    assert(use->is_Region(), "what else?");
    uint idx = 1;
    for (; use->in(idx) != proj; idx++);
    for (DUIterator_Fast imax, i = use->fast_outs(imax); i < imax; i++) {
      Node* u = use->fast_out(i);
      if (u->is_Phi() && phase->get_ctrl(u->in(idx)) == proj) {
        uses.push(u->in(idx));
      }
    }
  }
  for(uint next = 0; next < uses.size(); next++ ) {
    Node *n = uses.at(next);
    assert(phase->get_ctrl(n) == proj, "bad control");
    phase->set_ctrl_and_loop(n, new_unc_ctrl);
    if (n->in(0) == proj) {
      phase->igvn().replace_input_of(n, 0, new_unc_ctrl);
    }
    for (uint i = 0; i < n->req(); i++) {
      Node* m = n->in(i);
      if (m != NULL && phase->has_ctrl(m) && phase->get_ctrl(m) == proj) {
        uses.push(m);
      }
    }
  }

  phase->igvn().rehash_node_delayed(use);
  int nb = use->replace_edge(proj, new_unc_ctrl);
  assert(nb == 1, "only use expected");
}

void ShenandoahBarrierC2Support::test_in_cset(Node*& ctrl, Node*& not_cset_ctrl, Node* val, Node* raw_mem, PhaseIdealLoop* phase) {
  Node* old_ctrl = ctrl;
  PhaseIterGVN& igvn = phase->igvn();

  Node* raw_val        = new (phase->C) CastP2XNode(old_ctrl, val);
  Node* cset_idx       = new (phase->C) URShiftXNode(raw_val, igvn.intcon(ShenandoahHeapRegion::region_size_bytes_shift_jint()));

  // Figure out the target cset address with raw pointer math.
  // This avoids matching AddP+LoadB that would emit inefficient code.
  // See JDK-8245465.
  Node* cset_addr_ptr  = igvn.makecon(TypeRawPtr::make(ShenandoahHeap::in_cset_fast_test_addr()));
  Node* cset_addr      = new (phase->C) CastP2XNode(old_ctrl, cset_addr_ptr);
  Node* cset_load_addr = new (phase->C) AddXNode(cset_addr, cset_idx);
  Node* cset_load_ptr  = new (phase->C) CastX2PNode(cset_load_addr);

  Node* cset_load      = new (phase->C) LoadBNode(old_ctrl, raw_mem, cset_load_ptr,
                                                  DEBUG_ONLY(phase->C->get_adr_type(Compile::AliasIdxRaw)) NOT_DEBUG(NULL),
                                                  TypeInt::BYTE, MemNode::unordered);
  Node* cset_cmp       = new (phase->C) CmpINode(cset_load, igvn.zerocon(T_INT));
  Node* cset_bool      = new (phase->C) BoolNode(cset_cmp, BoolTest::eq);

  IfNode* cset_iff     = new (phase->C) IfNode(old_ctrl, cset_bool, PROB_UNLIKELY(0.999), COUNT_UNKNOWN);
  not_cset_ctrl        = new (phase->C) IfTrueNode(cset_iff);
  ctrl                 = new (phase->C) IfFalseNode(cset_iff);

  IdealLoopTree* loop = phase->get_loop(old_ctrl);
  phase->register_control(cset_iff,      loop, old_ctrl);
  phase->register_control(not_cset_ctrl, loop, cset_iff);
  phase->register_control(ctrl,          loop, cset_iff);

  phase->set_ctrl(cset_addr_ptr, phase->C->root());

  phase->register_new_node(raw_val,        old_ctrl);
  phase->register_new_node(cset_idx,       old_ctrl);
  phase->register_new_node(cset_addr,      old_ctrl);
  phase->register_new_node(cset_load_addr, old_ctrl);
  phase->register_new_node(cset_load_ptr,  old_ctrl);
  phase->register_new_node(cset_load,      old_ctrl);
  phase->register_new_node(cset_cmp,       old_ctrl);
  phase->register_new_node(cset_bool,      old_ctrl);
}

void ShenandoahBarrierC2Support::call_lrb_stub(Node*& ctrl, Node*& val, Node*& result_mem, Node* raw_mem, PhaseIdealLoop* phase) {
  IdealLoopTree*loop = phase->get_loop(ctrl);
  const TypePtr* obj_type = phase->igvn().type(val)->is_oopptr();

  // The slow path stub consumes and produces raw memory in addition
  // to the existing memory edges
  Node* base = find_bottom_mem(ctrl, phase);
  MergeMemNode* mm = MergeMemNode::make(phase->C, base);
  mm->set_memory_at(Compile::AliasIdxRaw, raw_mem);
  phase->register_new_node(mm, ctrl);

  Node* call = new (phase->C) CallLeafNode(ShenandoahBarrierSetC2::shenandoah_load_reference_barrier_Type(),
                                           CAST_FROM_FN_PTR(address, ShenandoahRuntime::load_reference_barrier),
                                           "shenandoah_load_reference_barrier", TypeRawPtr::BOTTOM);
  call->init_req(TypeFunc::Control, ctrl);
  call->init_req(TypeFunc::I_O, phase->C->top());
  call->init_req(TypeFunc::Memory, mm);
  call->init_req(TypeFunc::FramePtr, phase->C->top());
  call->init_req(TypeFunc::ReturnAdr, phase->C->top());
  call->init_req(TypeFunc::Parms, val);
  phase->register_control(call, loop, ctrl);
  ctrl = new (phase->C) ProjNode(call, TypeFunc::Control);
  phase->register_control(ctrl, loop, call);
  result_mem = new (phase->C) ProjNode(call, TypeFunc::Memory);
  phase->register_new_node(result_mem, call);
  val = new (phase->C) ProjNode(call, TypeFunc::Parms);
  phase->register_new_node(val, call);
  val = new (phase->C) CheckCastPPNode(ctrl, val, obj_type);
  phase->register_new_node(val, ctrl);
}

void ShenandoahBarrierC2Support::fix_ctrl(Node* barrier, Node* region, const MemoryGraphFixer& fixer, Unique_Node_List& uses, Unique_Node_List& uses_to_ignore, uint last, PhaseIdealLoop* phase) {
  Node* ctrl = phase->get_ctrl(barrier);
  Node* init_raw_mem = fixer.find_mem(ctrl, barrier);

  // Update the control of all nodes that should be after the
  // barrier control flow
  uses.clear();
  // Every node that is control dependent on the barrier's input
  // control will be after the expanded barrier. The raw memory (if
  // its memory is control dependent on the barrier's input control)
  // must stay above the barrier.
  uses_to_ignore.clear();
  if (phase->has_ctrl(init_raw_mem) && phase->get_ctrl(init_raw_mem) == ctrl && !init_raw_mem->is_Phi()) {
    uses_to_ignore.push(init_raw_mem);
  }
  for (uint next = 0; next < uses_to_ignore.size(); next++) {
    Node *n = uses_to_ignore.at(next);
    for (uint i = 0; i < n->req(); i++) {
      Node* in = n->in(i);
      if (in != NULL && phase->has_ctrl(in) && phase->get_ctrl(in) == ctrl) {
        uses_to_ignore.push(in);
      }
    }
  }
  for (DUIterator_Fast imax, i = ctrl->fast_outs(imax); i < imax; i++) {
    Node* u = ctrl->fast_out(i);
    if (u->_idx < last &&
        u != barrier &&
        !uses_to_ignore.member(u) &&
        (u->in(0) != ctrl || (!u->is_Region() && !u->is_Phi())) &&
        (ctrl->Opcode() != Op_CatchProj || u->Opcode() != Op_CreateEx)) {
      Node* old_c = phase->ctrl_or_self(u);
      Node* c = old_c;
      if (c != ctrl ||
          is_dominator_same_ctrl(old_c, barrier, u, phase) ||
          ShenandoahBarrierSetC2::is_shenandoah_state_load(u)) {
        phase->igvn().rehash_node_delayed(u);
        int nb = u->replace_edge(ctrl, region);
        if (u->is_CFG()) {
          if (phase->idom(u) == ctrl) {
            phase->set_idom(u, region, phase->dom_depth(region));
          }
        } else if (phase->get_ctrl(u) == ctrl) {
          assert(u != init_raw_mem, "should leave input raw mem above the barrier");
          uses.push(u);
        }
        assert(nb == 1, "more than 1 ctrl input?");
        --i, imax -= nb;
      }
    }
  }
}

static Node* create_phis_on_call_return(Node* ctrl, Node* c, Node* n, Node* n_clone, const CallProjections& projs, PhaseIdealLoop* phase) {
  Node* region = NULL;
  while (c != ctrl) {
    if (c->is_Region()) {
      region = c;
    }
    c = phase->idom(c);
  }
  assert(region != NULL, "");
  if (n->is_Bool()) {
    Node* bol_clone = n->clone();
    n = n->in(1);
    n_clone = n_clone->in(1);
    assert(n->is_Cmp() && n_clone->is_Cmp(), "should be cmp");
    Node* cmp_clone = n->clone();
    bol_clone->set_req(1, cmp_clone);
    if (n->in(1) != n_clone->in(1)) {
      cmp_clone->set_req(1, create_phis_on_call_return(ctrl, region, n->in(1), n_clone->in(1), projs, phase));
    }
    if (n->in(2) != n_clone->in(2)) {
      cmp_clone->set_req(2, create_phis_on_call_return(ctrl, region, n->in(2), n_clone->in(2), projs, phase));
    }
    phase->register_new_node(cmp_clone, region);
    phase->register_new_node(bol_clone, region);
    return bol_clone;
  }
  Node* phi = new (phase->C) PhiNode(region, n->bottom_type());
  for (uint j = 1; j < region->req(); j++) {
    Node* in = region->in(j);
    if (phase->is_dominator(projs.fallthrough_catchproj, in)) {
      phi->init_req(j, n);
    } else if (phase->is_dominator(projs.catchall_catchproj, in)) {
      phi->init_req(j, n_clone);
    } else {
      phi->init_req(j, create_phis_on_call_return(ctrl, in, n, n_clone, projs, phase));
    }
  }
  phase->register_new_node(phi, region);
  return phi;
}

void ShenandoahBarrierC2Support::pin_and_expand(PhaseIdealLoop* phase) {

  Unique_Node_List uses;
  Node_Stack stack(0);
  Node_List clones;
  for (int i = phase->C->shenandoah_barriers_count() - 1; i >= 0; i--) {
    ShenandoahLoadReferenceBarrierNode* lrb = phase->C->shenandoah_barrier(i);
    if (lrb->get_barrier_strength() == ShenandoahLoadReferenceBarrierNode::NONE) {
      continue;
    }

    Node* ctrl = phase->get_ctrl(lrb);
    Node* val = lrb->in(ShenandoahLoadReferenceBarrierNode::ValueIn);

    CallStaticJavaNode* unc = NULL;
    Node* unc_ctrl = NULL;
    Node* uncasted_val = val;

    for (DUIterator_Fast imax, i = lrb->fast_outs(imax); i < imax; i++) {
      Node* u = lrb->fast_out(i);
      if (u->Opcode() == Op_CastPP &&
          u->in(0) != NULL &&
          phase->is_dominator(u->in(0), ctrl)) {
        const Type* u_t = phase->igvn().type(u);

        if (u_t->meet(TypePtr::NULL_PTR) != u_t &&
            u->in(0)->Opcode() == Op_IfTrue &&
            u->in(0)->as_Proj()->is_uncommon_trap_if_pattern(Deoptimization::Reason_none) &&
            u->in(0)->in(0)->is_If() &&
            u->in(0)->in(0)->in(1)->Opcode() == Op_Bool &&
            u->in(0)->in(0)->in(1)->as_Bool()->_test._test == BoolTest::ne &&
            u->in(0)->in(0)->in(1)->in(1)->Opcode() == Op_CmpP &&
            u->in(0)->in(0)->in(1)->in(1)->in(1) == val &&
            u->in(0)->in(0)->in(1)->in(1)->in(2)->bottom_type() == TypePtr::NULL_PTR) {
          IdealLoopTree* loop = phase->get_loop(ctrl);
          IdealLoopTree* unc_loop = phase->get_loop(u->in(0));

          if (!unc_loop->is_member(loop)) {
            continue;
          }

          Node* branch = no_branches(ctrl, u->in(0), false, phase);
          assert(branch == NULL || branch == NodeSentinel, "was not looking for a branch");
          if (branch == NodeSentinel) {
            continue;
          }

          phase->igvn().replace_input_of(u, 1, val);
          phase->igvn().replace_input_of(lrb, ShenandoahLoadReferenceBarrierNode::ValueIn, u);
          phase->set_ctrl(u, u->in(0));
          phase->set_ctrl(lrb, u->in(0));
          unc = u->in(0)->as_Proj()->is_uncommon_trap_if_pattern(Deoptimization::Reason_none);
          unc_ctrl = u->in(0);
          val = u;

          for (DUIterator_Fast jmax, j = val->fast_outs(jmax); j < jmax; j++) {
            Node* u = val->fast_out(j);
            if (u == lrb) continue;
            phase->igvn().rehash_node_delayed(u);
            int nb = u->replace_edge(val, lrb);
            --j; jmax -= nb;
          }

          RegionNode* r = new (phase->C) RegionNode(3);
          IfNode* iff = unc_ctrl->in(0)->as_If();

          Node* ctrl_use = unc_ctrl->unique_ctrl_out();
          Node* unc_ctrl_clone = unc_ctrl->clone();
          phase->register_control(unc_ctrl_clone, loop, iff);
          Node* c = unc_ctrl_clone;
          Node* new_cast = clone_null_check(c, val, unc_ctrl_clone, phase);
          r->init_req(1, new_cast->in(0)->in(0)->as_If()->proj_out(0));

          phase->igvn().replace_input_of(unc_ctrl, 0, c->in(0));
          phase->set_idom(unc_ctrl, c->in(0), phase->dom_depth(unc_ctrl));
          phase->lazy_replace(c, unc_ctrl);
          c = NULL;;
          phase->igvn().replace_input_of(val, 0, unc_ctrl_clone);
          phase->set_ctrl(val, unc_ctrl_clone);

          IfNode* new_iff = new_cast->in(0)->in(0)->as_If();
          fix_null_check(unc, unc_ctrl_clone, r, uses, phase);
          Node* iff_proj = iff->proj_out(0);
          r->init_req(2, iff_proj);
          phase->register_control(r, phase->ltree_root(), iff);

          Node* new_bol = new_iff->in(1)->clone();
          Node* new_cmp = new_bol->in(1)->clone();
          assert(new_cmp->Opcode() == Op_CmpP, "broken");
          assert(new_cmp->in(1) == val->in(1), "broken");
          new_bol->set_req(1, new_cmp);
          new_cmp->set_req(1, lrb);
          phase->register_new_node(new_bol, new_iff->in(0));
          phase->register_new_node(new_cmp, new_iff->in(0));
          phase->igvn().replace_input_of(new_iff, 1, new_bol);
          phase->igvn().replace_input_of(new_cast, 1, lrb);

          for (DUIterator_Fast imax, i = lrb->fast_outs(imax); i < imax; i++) {
            Node* u = lrb->fast_out(i);
            if (u == new_cast || u == new_cmp) {
              continue;
            }
            phase->igvn().rehash_node_delayed(u);
            int nb = u->replace_edge(lrb, new_cast);
            assert(nb > 0, "no update?");
            --i; imax -= nb;
          }

          for (DUIterator_Fast imax, i = val->fast_outs(imax); i < imax; i++) {
            Node* u = val->fast_out(i);
            if (u == lrb) {
              continue;
            }
            phase->igvn().rehash_node_delayed(u);
            int nb = u->replace_edge(val, new_cast);
            assert(nb > 0, "no update?");
            --i; imax -= nb;
          }

          ctrl = unc_ctrl_clone;
          phase->set_ctrl_and_loop(lrb, ctrl);
          break;
        }
      }
    }
    if ((ctrl->is_Proj() && ctrl->in(0)->is_CallJava()) || ctrl->is_CallJava()) {
      CallNode* call = ctrl->is_Proj() ? ctrl->in(0)->as_CallJava() : ctrl->as_CallJava();
      if (call->entry_point() == OptoRuntime::rethrow_stub()) {
        // The rethrow call may have too many projections to be
        // properly handled here. Given there's no reason for a
        // barrier to depend on the call, move it above the call
        if (phase->get_ctrl(val) == ctrl) {
          assert(val->Opcode() == Op_DecodeN, "unexpected node");
          assert(phase->is_dominator(phase->get_ctrl(val->in(1)), call->in(0)), "Load is too low");
          phase->set_ctrl(val, call->in(0));
        }
        phase->set_ctrl(lrb, call->in(0));
        continue;
      }
      CallProjections projs;
      call->extract_projections(&projs, false, false);

      Node* lrb_clone = lrb->clone();
      phase->register_new_node(lrb_clone, projs.catchall_catchproj);
      phase->set_ctrl(lrb, projs.fallthrough_catchproj);

      stack.push(lrb, 0);
      clones.push(lrb_clone);

      do {
        assert(stack.size() == clones.size(), "");
        Node* n = stack.node();
#ifdef ASSERT
        if (n->is_Load()) {
          Node* mem = n->in(MemNode::Memory);
          for (DUIterator_Fast jmax, j = mem->fast_outs(jmax); j < jmax; j++) {
            Node* u = mem->fast_out(j);
            assert(!u->is_Store() || !u->is_LoadStore() || phase->get_ctrl(u) != ctrl, "anti dependent store?");
          }
        }
#endif
        uint idx = stack.index();
        Node* n_clone = clones.at(clones.size()-1);
        if (idx < n->outcnt()) {
          Node* u = n->raw_out(idx);
          Node* c = phase->ctrl_or_self(u);
          if (phase->is_dominator(call, c) && phase->is_dominator(c, projs.fallthrough_proj)) {
            stack.set_index(idx+1);
            assert(!u->is_CFG(), "");
            stack.push(u, 0);
            Node* u_clone = u->clone();
            int nb = u_clone->replace_edge(n, n_clone);
            assert(nb > 0, "should have replaced some uses");
            phase->register_new_node(u_clone, projs.catchall_catchproj);
            clones.push(u_clone);
            phase->set_ctrl(u, projs.fallthrough_catchproj);
          } else {
            bool replaced = false;
            if (u->is_Phi()) {
              for (uint k = 1; k < u->req(); k++) {
                if (u->in(k) == n) {
                  if (phase->is_dominator(projs.catchall_catchproj, u->in(0)->in(k))) {
                    phase->igvn().replace_input_of(u, k, n_clone);
                    replaced = true;
                  } else if (!phase->is_dominator(projs.fallthrough_catchproj, u->in(0)->in(k))) {
                    phase->igvn().replace_input_of(u, k, create_phis_on_call_return(ctrl, u->in(0)->in(k), n, n_clone, projs, phase));
                    replaced = true;
                  }
                }
              }
            } else {
              if (phase->is_dominator(projs.catchall_catchproj, c)) {
                phase->igvn().rehash_node_delayed(u);
                int nb = u->replace_edge(n, n_clone);
                assert(nb > 0, "should have replaced some uses");
                replaced = true;
              } else if (!phase->is_dominator(projs.fallthrough_catchproj, c)) {
                phase->igvn().rehash_node_delayed(u);
                int nb = u->replace_edge(n, create_phis_on_call_return(ctrl, c, n, n_clone, projs, phase));
                assert(nb > 0, "should have replaced some uses");
                replaced = true;
              }
            }
            if (!replaced) {
              stack.set_index(idx+1);
            }
          }
        } else {
          stack.pop();
          clones.pop();
        }
      } while (stack.size() > 0);
      assert(stack.size() == 0 && clones.size() == 0, "");
    }
  }

  // Expand load-reference-barriers
  MemoryGraphFixer fixer(Compile::AliasIdxRaw, true, phase);
  Unique_Node_List uses_to_ignore;
  for (int i = phase->C->shenandoah_barriers_count() - 1; i >= 0; i--) {
    ShenandoahLoadReferenceBarrierNode* lrb = phase->C->shenandoah_barrier(i);
    if (lrb->get_barrier_strength() == ShenandoahLoadReferenceBarrierNode::NONE) {
      phase->igvn().replace_node(lrb, lrb->in(ShenandoahLoadReferenceBarrierNode::ValueIn));
      continue;
    }
    uint last = phase->C->unique();
    Node* ctrl = phase->get_ctrl(lrb);
    Node* val = lrb->in(ShenandoahLoadReferenceBarrierNode::ValueIn);


    Node* orig_ctrl = ctrl;

    Node* raw_mem = fixer.find_mem(ctrl, lrb);
    Node* init_raw_mem = raw_mem;
    Node* raw_mem_for_ctrl = fixer.find_mem(ctrl, NULL);

    IdealLoopTree *loop = phase->get_loop(ctrl);
    CallStaticJavaNode* unc = lrb->pin_and_expand_null_check(phase->igvn());
    Node* unc_ctrl = NULL;
    if (unc != NULL) {
      if (val->in(ShenandoahLoadReferenceBarrierNode::Control) != ctrl) {
        unc = NULL;
      } else {
        unc_ctrl = val->in(ShenandoahLoadReferenceBarrierNode::Control);
      }
    }

    Node* uncasted_val = val;
    if (unc != NULL) {
      uncasted_val = val->in(1);
    }

    Node* heap_stable_ctrl = NULL;
    Node* null_ctrl = NULL;

    assert(val->bottom_type()->make_oopptr(), "need oop");
    assert(val->bottom_type()->make_oopptr()->const_oop() == NULL, "expect non-constant");

    enum { _heap_stable = 1, _not_cset, _fwded, _evac_path, _null_path, PATH_LIMIT };
    Node* region = new (phase->C) RegionNode(PATH_LIMIT);
    Node* val_phi = new (phase->C) PhiNode(region, uncasted_val->bottom_type()->is_oopptr());
    Node* raw_mem_phi = PhiNode::make(region, raw_mem, Type::MEMORY, TypeRawPtr::BOTTOM);

    // Stable path.
    test_gc_state(ctrl, raw_mem, heap_stable_ctrl, phase, ShenandoahHeap::HAS_FORWARDED);
    IfNode* heap_stable_iff = heap_stable_ctrl->in(0)->as_If();

    // Heap stable case
    region->init_req(_heap_stable, heap_stable_ctrl);
    val_phi->init_req(_heap_stable, uncasted_val);
    raw_mem_phi->init_req(_heap_stable, raw_mem);

    Node* reg2_ctrl = NULL;
    // Null case
    test_null(ctrl, val, null_ctrl, phase);
    if (null_ctrl != NULL) {
      reg2_ctrl = null_ctrl->in(0);
      region->init_req(_null_path, null_ctrl);
      val_phi->init_req(_null_path, uncasted_val);
      raw_mem_phi->init_req(_null_path, raw_mem);
    } else {
      region->del_req(_null_path);
      val_phi->del_req(_null_path);
      raw_mem_phi->del_req(_null_path);
    }

    // Test for in-cset.
    // Wires !in_cset(obj) to slot 2 of region and phis
    Node* not_cset_ctrl = NULL;
    test_in_cset(ctrl, not_cset_ctrl, uncasted_val, raw_mem, phase);
    if (not_cset_ctrl != NULL) {
      if (reg2_ctrl == NULL) reg2_ctrl = not_cset_ctrl->in(0);
      region->init_req(_not_cset, not_cset_ctrl);
      val_phi->init_req(_not_cset, uncasted_val);
      raw_mem_phi->init_req(_not_cset, raw_mem);
    }

    // Resolve object when orig-value is in cset.
    // Make the unconditional resolve for fwdptr.
    Node* new_val = uncasted_val;
    if (unc_ctrl != NULL) {
      // Clone the null check in this branch to allow implicit null check
      new_val = clone_null_check(ctrl, val, unc_ctrl, phase);
      fix_null_check(unc, unc_ctrl, ctrl->in(0)->as_If()->proj_out(0), uses, phase);

      IfNode* iff = unc_ctrl->in(0)->as_If();
      phase->igvn().replace_input_of(iff, 1, phase->igvn().intcon(1));
    }
    Node* addr = new (phase->C) AddPNode(new_val, uncasted_val, phase->igvn().MakeConX(oopDesc::mark_offset_in_bytes()));
    phase->register_new_node(addr, ctrl);
    assert(new_val->bottom_type()->isa_oopptr(), "what else?");
    Node* markword = new (phase->C) LoadXNode(ctrl, raw_mem, addr, TypeRawPtr::BOTTOM, TypeX_X, MemNode::unordered);
    phase->register_new_node(markword, ctrl);

    // Test if object is forwarded. This is the case if lowest two bits are set.
    Node* masked = new (phase->C) AndXNode(markword, phase->igvn().MakeConX(markOopDesc::lock_mask_in_place));
    phase->register_new_node(masked, ctrl);
    Node* cmp = new (phase->C) CmpXNode(masked, phase->igvn().MakeConX(markOopDesc::marked_value));
    phase->register_new_node(cmp, ctrl);

    // Only branch to LRB stub if object is not forwarded; otherwise reply with fwd ptr
    Node* bol = new (phase->C) BoolNode(cmp, BoolTest::eq); // Equals 3 means it's forwarded
    phase->register_new_node(bol, ctrl);

    IfNode* iff = new (phase->C) IfNode(ctrl, bol, PROB_LIKELY(0.999), COUNT_UNKNOWN);
    phase->register_control(iff, loop, ctrl);
    Node* if_fwd = new (phase->C) IfTrueNode(iff);
    phase->register_control(if_fwd, loop, iff);
    Node* if_not_fwd = new (phase->C) IfFalseNode(iff);
    phase->register_control(if_not_fwd, loop, iff);

    // Decode forward pointer: since we already have the lowest bits, we can just subtract them
    // from the mark word without the need for large immediate mask.
    Node* masked2 = new (phase->C) SubXNode(markword, masked);
    phase->register_new_node(masked2, if_fwd);
    Node* fwdraw = new (phase->C) CastX2PNode(masked2);
    fwdraw->init_req(0, if_fwd);
    phase->register_new_node(fwdraw, if_fwd);
    Node* fwd = new (phase->C) CheckCastPPNode(NULL, fwdraw, val->bottom_type());
    phase->register_new_node(fwd, if_fwd);

    // Wire up not-equal-path in slots 3.
    region->init_req(_fwded, if_fwd);
    val_phi->init_req(_fwded, fwd);
    raw_mem_phi->init_req(_fwded, raw_mem);

    // Call lrb-stub and wire up that path in slots 4
    Node* result_mem = NULL;
    ctrl = if_not_fwd;
    fwd = new_val;
    call_lrb_stub(ctrl, fwd, result_mem, raw_mem, phase);
    region->init_req(_evac_path, ctrl);
    val_phi->init_req(_evac_path, fwd);
    raw_mem_phi->init_req(_evac_path, result_mem);

    phase->register_control(region, loop, heap_stable_iff);
    Node* out_val = val_phi;
    phase->register_new_node(val_phi, region);
    phase->register_new_node(raw_mem_phi, region);

    fix_ctrl(lrb, region, fixer, uses, uses_to_ignore, last, phase);

    ctrl = orig_ctrl;

    if (unc != NULL) {
      for (DUIterator_Fast imax, i = val->fast_outs(imax); i < imax; i++) {
        Node* u = val->fast_out(i);
        Node* c = phase->ctrl_or_self(u);
        if (u != lrb && (c != ctrl || is_dominator_same_ctrl(c, lrb, u, phase))) {
          phase->igvn().rehash_node_delayed(u);
          int nb = u->replace_edge(val, out_val);
          --i, imax -= nb;
        }
      }
      if (val->outcnt() == 0) {
        phase->igvn()._worklist.push(val);
      }
    }
    phase->igvn().replace_node(lrb, out_val);

    follow_barrier_uses(out_val, ctrl, uses, phase);

    for(uint next = 0; next < uses.size(); next++ ) {
      Node *n = uses.at(next);
      assert(phase->get_ctrl(n) == ctrl, "bad control");
      assert(n != init_raw_mem, "should leave input raw mem above the barrier");
      phase->set_ctrl(n, region);
      follow_barrier_uses(n, ctrl, uses, phase);
    }

    // The slow path call produces memory: hook the raw memory phi
    // from the expanded load reference barrier with the rest of the graph
    // which may require adding memory phis at every post dominated
    // region and at enclosing loop heads. Use the memory state
    // collected in memory_nodes to fix the memory graph. Update that
    // memory state as we go.
    fixer.fix_mem(ctrl, region, init_raw_mem, raw_mem_for_ctrl, raw_mem_phi, uses);
  }
  // Done expanding load-reference-barriers.
  assert(phase->C->shenandoah_barriers_count() == 0, "all load reference barrier nodes should have been replaced");

}

void ShenandoahBarrierC2Support::move_gc_state_test_out_of_loop(IfNode* iff, PhaseIdealLoop* phase) {
  IdealLoopTree *loop = phase->get_loop(iff);
  Node* loop_head = loop->_head;
  Node* entry_c = loop_head->in(LoopNode::EntryControl);

  Node* bol = iff->in(1);
  Node* cmp = bol->in(1);
  Node* andi = cmp->in(1);
  Node* load = andi->in(1);

  assert(is_gc_state_load(load), "broken");
  if (!phase->is_dominator(load->in(0), entry_c)) {
    Node* mem_ctrl = NULL;
    Node* mem = dom_mem(load->in(MemNode::Memory), loop_head, Compile::AliasIdxRaw, mem_ctrl, phase);
    load = load->clone();
    load->set_req(MemNode::Memory, mem);
    load->set_req(0, entry_c);
    phase->register_new_node(load, entry_c);
    andi = andi->clone();
    andi->set_req(1, load);
    phase->register_new_node(andi, entry_c);
    cmp = cmp->clone();
    cmp->set_req(1, andi);
    phase->register_new_node(cmp, entry_c);
    bol = bol->clone();
    bol->set_req(1, cmp);
    phase->register_new_node(bol, entry_c);

    Node* old_bol =iff->in(1);
    phase->igvn().replace_input_of(iff, 1, bol);
  }
}

bool ShenandoahBarrierC2Support::identical_backtoback_ifs(Node* n, PhaseIdealLoop* phase) {
  if (!n->is_If() || n->is_CountedLoopEnd()) {
    return false;
  }
  Node* region = n->in(0);

  if (!region->is_Region()) {
    return false;
  }
  Node* dom = phase->idom(region);
  if (!dom->is_If()) {
    return false;
  }

  if (!is_heap_stable_test(n) || !is_heap_stable_test(dom)) {
    return false;
  }

  IfNode* dom_if = dom->as_If();
  Node* proj_true = dom_if->proj_out(1);
  Node* proj_false = dom_if->proj_out(0);

  for (uint i = 1; i < region->req(); i++) {
    if (phase->is_dominator(proj_true, region->in(i))) {
      continue;
    }
    if (phase->is_dominator(proj_false, region->in(i))) {
      continue;
    }
    return false;
  }

  return true;
}

static bool merge_point_too_heavy(Compile* C, Node* region) {
  // Bail out if the region and its phis have too many users.
  int weight = 0;
  for (DUIterator_Fast imax, i = region->fast_outs(imax); i < imax; i++) {
    weight += region->fast_out(i)->outcnt();
  }
  int nodes_left = C->max_node_limit() - C->live_nodes();
  if (weight * 8 > nodes_left) {
#ifndef PRODUCT
    if (PrintOpto) {
      tty->print_cr("*** Split-if bails out:  %d nodes, region weight %d", C->unique(), weight);
    }
#endif
    return true;
  } else {
    return false;
  }
}

static bool merge_point_safe(Node* region) {
  // 4799512: Stop split_if_with_blocks from splitting a block with a ConvI2LNode
  // having a PhiNode input. This sidesteps the dangerous case where the split
  // ConvI2LNode may become TOP if the input Value() does not
  // overlap the ConvI2L range, leaving a node which may not dominate its
  // uses.
  // A better fix for this problem can be found in the BugTraq entry, but
  // expediency for Mantis demands this hack.
  // 6855164: If the merge point has a FastLockNode with a PhiNode input, we stop
  // split_if_with_blocks from splitting a block because we could not move around
  // the FastLockNode.
  for (DUIterator_Fast imax, i = region->fast_outs(imax); i < imax; i++) {
    Node* n = region->fast_out(i);
    if (n->is_Phi()) {
      for (DUIterator_Fast jmax, j = n->fast_outs(jmax); j < jmax; j++) {
        Node* m = n->fast_out(j);
        if (m->is_FastLock())
          return false;
#ifdef _LP64
        if (m->Opcode() == Op_ConvI2L)
          return false;
        if (m->is_CastII() && m->isa_CastII()->has_range_check()) {
          return false;
        }
#endif
      }
    }
  }
  return true;
}

static bool can_split_if(PhaseIdealLoop* phase, Node* n_ctrl) {
  if (phase->C->live_nodes() > 35000) {
    return false; // Method too big
  }

  // Do not do 'split-if' if irreducible loops are present.
  if (phase->_has_irreducible_loops) {
    return false;
  }

  if (merge_point_too_heavy(phase->C, n_ctrl)) {
    return false;
  }

  // Do not do 'split-if' if some paths are dead.  First do dead code
  // elimination and then see if its still profitable.
  for (uint i = 1; i < n_ctrl->req(); i++) {
    if (n_ctrl->in(i) == phase->C->top()) {
      return false;
    }
  }

  // If trying to do a 'Split-If' at the loop head, it is only
  // profitable if the cmp folds up on BOTH paths.  Otherwise we
  // risk peeling a loop forever.

  // CNC - Disabled for now.  Requires careful handling of loop
  // body selection for the cloned code.  Also, make sure we check
  // for any input path not being in the same loop as n_ctrl.  For
  // irreducible loops we cannot check for 'n_ctrl->is_Loop()'
  // because the alternative loop entry points won't be converted
  // into LoopNodes.
  IdealLoopTree *n_loop = phase->get_loop(n_ctrl);
  for (uint j = 1; j < n_ctrl->req(); j++) {
    if (phase->get_loop(n_ctrl->in(j)) != n_loop) {
      return false;
    }
  }

  // Check for safety of the merge point.
  if (!merge_point_safe(n_ctrl)) {
    return false;
  }

  return true;
}

void ShenandoahBarrierC2Support::merge_back_to_back_tests(Node* n, PhaseIdealLoop* phase) {
  assert(is_heap_stable_test(n), "no other tests");
  if (identical_backtoback_ifs(n, phase)) {
    Node* n_ctrl = n->in(0);
    if (can_split_if(phase, n_ctrl)) {
      IfNode* dom_if = phase->idom(n_ctrl)->as_If();
      if (is_heap_stable_test(n)) {
        Node* gc_state_load = n->in(1)->in(1)->in(1)->in(1);
        assert(is_gc_state_load(gc_state_load), "broken");
        Node* dom_gc_state_load = dom_if->in(1)->in(1)->in(1)->in(1);
        assert(is_gc_state_load(dom_gc_state_load), "broken");
        if (gc_state_load != dom_gc_state_load) {
          phase->igvn().replace_node(gc_state_load, dom_gc_state_load);
        }
      }
      PhiNode* bolphi = PhiNode::make_blank(n_ctrl, n->in(1));
      Node* proj_true = dom_if->proj_out(1);
      Node* proj_false = dom_if->proj_out(0);
      Node* con_true = phase->igvn().makecon(TypeInt::ONE);
      Node* con_false = phase->igvn().makecon(TypeInt::ZERO);

      for (uint i = 1; i < n_ctrl->req(); i++) {
        if (phase->is_dominator(proj_true, n_ctrl->in(i))) {
          bolphi->init_req(i, con_true);
        } else {
          assert(phase->is_dominator(proj_false, n_ctrl->in(i)), "bad if");
          bolphi->init_req(i, con_false);
        }
      }
      phase->register_new_node(bolphi, n_ctrl);
      phase->igvn().replace_input_of(n, 1, bolphi);
      phase->do_split_if(n);
    }
  }
}

IfNode* ShenandoahBarrierC2Support::find_unswitching_candidate(const IdealLoopTree* loop, PhaseIdealLoop* phase) {
  // Find first invariant test that doesn't exit the loop
  LoopNode *head = loop->_head->as_Loop();
  IfNode* unswitch_iff = NULL;
  Node* n = head->in(LoopNode::LoopBackControl);
  int loop_has_sfpts = -1;
  while (n != head) {
    Node* n_dom = phase->idom(n);
    if (n->is_Region()) {
      if (n_dom->is_If()) {
        IfNode* iff = n_dom->as_If();
        if (iff->in(1)->is_Bool()) {
          BoolNode* bol = iff->in(1)->as_Bool();
          if (bol->in(1)->is_Cmp()) {
            // If condition is invariant and not a loop exit,
            // then found reason to unswitch.
            if (is_heap_stable_test(iff) &&
                (loop_has_sfpts == -1 || loop_has_sfpts == 0)) {
              assert(!loop->is_loop_exit(iff), "both branches should be in the loop");
              if (loop_has_sfpts == -1) {
                for(uint i = 0; i < loop->_body.size(); i++) {
                  Node *m = loop->_body[i];
                  if (m->is_SafePoint() && !m->is_CallLeaf()) {
                    loop_has_sfpts = 1;
                    break;
                  }
                }
                if (loop_has_sfpts == -1) {
                  loop_has_sfpts = 0;
                }
              }
              if (!loop_has_sfpts) {
                unswitch_iff = iff;
              }
            }
          }
        }
      }
    }
    n = n_dom;
  }
  return unswitch_iff;
}


void ShenandoahBarrierC2Support::optimize_after_expansion(VectorSet &visited, Node_Stack &stack, Node_List &old_new, PhaseIdealLoop* phase) {
  Node_List heap_stable_tests;
  stack.push(phase->C->start(), 0);
  do {
    Node* n = stack.node();
    uint i = stack.index();

    if (i < n->outcnt()) {
      Node* u = n->raw_out(i);
      stack.set_index(i+1);
      if (!visited.test_set(u->_idx)) {
        stack.push(u, 0);
      }
    } else {
      stack.pop();
      if (n->is_If() && is_heap_stable_test(n)) {
        heap_stable_tests.push(n);
      }
    }
  } while (stack.size() > 0);

  for (uint i = 0; i < heap_stable_tests.size(); i++) {
    Node* n = heap_stable_tests.at(i);
    assert(is_heap_stable_test(n), "only evacuation test");
    merge_back_to_back_tests(n, phase);
  }

  if (!phase->C->major_progress()) {
    VectorSet seen(Thread::current()->resource_area());
    for (uint i = 0; i < heap_stable_tests.size(); i++) {
      Node* n = heap_stable_tests.at(i);
      IdealLoopTree* loop = phase->get_loop(n);
      if (loop != phase->ltree_root() &&
          loop->_child == NULL &&
          !loop->_irreducible) {
        LoopNode* head = loop->_head->as_Loop();
        if ((!head->is_CountedLoop() || head->as_CountedLoop()->is_main_loop() || head->as_CountedLoop()->is_normal_loop()) &&
            !seen.test_set(head->_idx)) {
          IfNode* iff = find_unswitching_candidate(loop, phase);
          if (iff != NULL) {
            Node* bol = iff->in(1);
            move_gc_state_test_out_of_loop(iff, phase);
            if (loop->policy_unswitching(phase)) {
              phase->do_unswitching(loop, old_new);
            } else {
              // Not proceeding with unswitching. Move load back in
              // the loop.
              phase->igvn().replace_input_of(iff, 1, bol);
            }
          }
        }
      }
    }
  }
}

#ifdef ASSERT
void ShenandoahBarrierC2Support::verify_raw_mem(RootNode* root) {
  const bool trace = false;
  ResourceMark rm;
  Unique_Node_List nodes;
  Unique_Node_List controls;
  Unique_Node_List memories;

  nodes.push(root);
  for (uint next = 0; next < nodes.size(); next++) {
    Node *n  = nodes.at(next);
    if (ShenandoahBarrierSetC2::is_shenandoah_lrb_call(n)) {
      controls.push(n);
      if (trace) { tty->print("XXXXXX verifying"); n->dump(); }
      for (uint next2 = 0; next2 < controls.size(); next2++) {
        Node *m = controls.at(next2);
        for (DUIterator_Fast imax, i = m->fast_outs(imax); i < imax; i++) {
          Node* u = m->fast_out(i);
          if (u->is_CFG() && !u->is_Root() &&
              !(u->Opcode() == Op_CProj && u->in(0)->Opcode() == Op_NeverBranch && u->as_Proj()->_con == 1) &&
              !(u->is_Region() && u->unique_ctrl_out()->Opcode() == Op_Halt)) {
            if (trace) { tty->print("XXXXXX pushing control"); u->dump(); }
            controls.push(u);
          }
        }
      }
      memories.push(n->as_Call()->proj_out(TypeFunc::Memory));
      for (uint next2 = 0; next2 < memories.size(); next2++) {
        Node *m = memories.at(next2);
        assert(m->bottom_type() == Type::MEMORY, "");
        for (DUIterator_Fast imax, i = m->fast_outs(imax); i < imax; i++) {
          Node* u = m->fast_out(i);
          if (u->bottom_type() == Type::MEMORY && (u->is_Mem() || u->is_ClearArray())) {
            if (trace) { tty->print("XXXXXX pushing memory"); u->dump(); }
            memories.push(u);
          } else if (u->is_LoadStore()) {
            if (trace) { tty->print("XXXXXX pushing memory"); u->find_out_with(Op_SCMemProj)->dump(); }
            memories.push(u->find_out_with(Op_SCMemProj));
          } else if (u->is_MergeMem() && u->as_MergeMem()->memory_at(Compile::AliasIdxRaw) == m) {
            if (trace) { tty->print("XXXXXX pushing memory"); u->dump(); }
            memories.push(u);
          } else if (u->is_Phi()) {
            assert(u->bottom_type() == Type::MEMORY, "");
            if (u->adr_type() == TypeRawPtr::BOTTOM || u->adr_type() == TypePtr::BOTTOM) {
              assert(controls.member(u->in(0)), "");
              if (trace) { tty->print("XXXXXX pushing memory"); u->dump(); }
              memories.push(u);
            }
          } else if (u->is_SafePoint() || u->is_MemBar()) {
            for (DUIterator_Fast jmax, j = u->fast_outs(jmax); j < jmax; j++) {
              Node* uu = u->fast_out(j);
              if (uu->bottom_type() == Type::MEMORY) {
                if (trace) { tty->print("XXXXXX pushing memory"); uu->dump(); }
                memories.push(uu);
              }
            }
          }
        }
      }
      for (uint next2 = 0; next2 < controls.size(); next2++) {
        Node *m = controls.at(next2);
        if (m->is_Region()) {
          bool all_in = true;
          for (uint i = 1; i < m->req(); i++) {
            if (!controls.member(m->in(i))) {
              all_in = false;
              break;
            }
          }
          if (trace) { tty->print("XXX verifying %s", all_in ? "all in" : ""); m->dump(); }
          bool found_phi = false;
          for (DUIterator_Fast jmax, j = m->fast_outs(jmax); j < jmax && !found_phi; j++) {
            Node* u = m->fast_out(j);
            if (u->is_Phi() && memories.member(u)) {
              found_phi = true;
              for (uint i = 1; i < u->req() && found_phi; i++) {
                Node* k = u->in(i);
                if (memories.member(k) != controls.member(m->in(i))) {
                  found_phi = false;
                }
              }
            }
          }
          assert(found_phi || all_in, "");
        }
      }
      controls.clear();
      memories.clear();
    }
    for( uint i = 0; i < n->len(); ++i ) {
      Node *m = n->in(i);
      if (m != NULL) {
        nodes.push(m);
      }
    }
  }
}
#endif

#ifdef ASSERT
static bool has_never_branch(Node* root) {
  for (uint i = 1; i < root->req(); i++) {
    Node* in = root->in(i);
    if (in != NULL && in->Opcode() == Op_Halt && in->in(0)->is_Proj() && in->in(0)->in(0)->Opcode() == Op_NeverBranch) {
      return true;
    }
  }
  return false;
}
#endif

void MemoryGraphFixer::collect_memory_nodes() {
  Node_Stack stack(0);
  VectorSet visited(Thread::current()->resource_area());
  Node_List regions;

  // Walk the raw memory graph and create a mapping from CFG node to
  // memory node. Exclude phis for now.
  stack.push(_phase->C->root(), 1);
  do {
    Node* n = stack.node();
    int opc = n->Opcode();
    uint i = stack.index();
    if (i < n->req()) {
      Node* mem = NULL;
      if (opc == Op_Root) {
        Node* in = n->in(i);
        int in_opc = in->Opcode();
        if (in_opc == Op_Return || in_opc == Op_Rethrow) {
          mem = in->in(TypeFunc::Memory);
        } else if (in_opc == Op_Halt) {
          if (!in->in(0)->is_Region()) {
            Node* proj = in->in(0);
            assert(proj->is_Proj(), "");
            Node* in = proj->in(0);
            assert(in->is_CallStaticJava() || in->Opcode() == Op_NeverBranch || in->Opcode() == Op_Catch || proj->is_IfProj(), "");
            if (in->is_CallStaticJava()) {
              mem = in->in(TypeFunc::Memory);
            } else if (in->Opcode() == Op_Catch) {
              Node* call = in->in(0)->in(0);
              assert(call->is_Call(), "");
              mem = call->in(TypeFunc::Memory);
            } else if (in->Opcode() == Op_NeverBranch) {
              ResourceMark rm;
              Unique_Node_List wq;
              wq.push(in);
              wq.push(in->as_Multi()->proj_out(0));
              for (uint j = 1; j < wq.size(); j++) {
                Node* c = wq.at(j);
                assert(!c->is_Root(), "shouldn't leave loop");
                if (c->is_SafePoint()) {
                  assert(mem == NULL, "only one safepoint");
                  mem = c->in(TypeFunc::Memory);
                }
                for (DUIterator_Fast kmax, k = c->fast_outs(kmax); k < kmax; k++) {
                  Node* u = c->fast_out(k);
                  if (u->is_CFG()) {
                    wq.push(u);
                  }
                }
              }
              assert(mem != NULL, "should have found safepoint");
            }
          }
        } else {
#ifdef ASSERT
          n->dump();
          in->dump();
#endif
          ShouldNotReachHere();
        }
      } else {
        assert(n->is_Phi() && n->bottom_type() == Type::MEMORY, "");
        assert(n->adr_type() == TypePtr::BOTTOM || _phase->C->get_alias_index(n->adr_type()) == _alias, "");
        mem = n->in(i);
      }
      i++;
      stack.set_index(i);
      if (mem == NULL) {
        continue;
      }
      for (;;) {
        if (visited.test_set(mem->_idx) || mem->is_Start()) {
          break;
        }
        if (mem->is_Phi()) {
          stack.push(mem, 2);
          mem = mem->in(1);
        } else if (mem->is_Proj()) {
          stack.push(mem, mem->req());
          mem = mem->in(0);
        } else if (mem->is_SafePoint() || mem->is_MemBar()) {
          mem = mem->in(TypeFunc::Memory);
        } else if (mem->is_MergeMem()) {
          MergeMemNode* mm = mem->as_MergeMem();
          mem = mm->memory_at(_alias);
        } else if (mem->is_Store() || mem->is_LoadStore() || mem->is_ClearArray()) {
          assert(_alias == Compile::AliasIdxRaw, "");
          stack.push(mem, mem->req());
          mem = mem->in(MemNode::Memory);
        } else {
#ifdef ASSERT
          mem->dump();
#endif
          ShouldNotReachHere();
        }
      }
    } else {
      if (n->is_Phi()) {
        // Nothing
      } else if (!n->is_Root()) {
        Node* c = get_ctrl(n);
        _memory_nodes.map(c->_idx, n);
      }
      stack.pop();
    }
  } while(stack.is_nonempty());

  // Iterate over CFG nodes in rpo and propagate memory state to
  // compute memory state at regions, creating new phis if needed.
  Node_List rpo_list;
  visited.Clear();
  _phase->rpo(_phase->C->root(), stack, visited, rpo_list);
  Node* root = rpo_list.pop();
  assert(root == _phase->C->root(), "");

  const bool trace = false;
#ifdef ASSERT
  if (trace) {
    for (int i = rpo_list.size() - 1; i >= 0; i--) {
      Node* c = rpo_list.at(i);
      if (_memory_nodes[c->_idx] != NULL) {
        tty->print("X %d", c->_idx);  _memory_nodes[c->_idx]->dump();
      }
    }
  }
#endif
  uint last = _phase->C->unique();

#ifdef ASSERT
  uint8_t max_depth = 0;
  for (LoopTreeIterator iter(_phase->ltree_root()); !iter.done(); iter.next()) {
    IdealLoopTree* lpt = iter.current();
    max_depth = MAX2(max_depth, lpt->_nest);
  }
#endif

  bool progress = true;
  int iteration = 0;
  Node_List dead_phis;
  while (progress) {
    progress = false;
    iteration++;
    assert(iteration <= 2+max_depth || _phase->C->has_irreducible_loop() || has_never_branch(_phase->C->root()), "");
    if (trace) { tty->print_cr("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"); }
    IdealLoopTree* last_updated_ilt = NULL;
    for (int i = rpo_list.size() - 1; i >= 0; i--) {
      Node* c = rpo_list.at(i);

      Node* prev_mem = _memory_nodes[c->_idx];
      if (c->is_Region()) {
        Node* prev_region = regions[c->_idx];
        Node* unique = NULL;
        for (uint j = 1; j < c->req() && unique != NodeSentinel; j++) {
          Node* m = _memory_nodes[c->in(j)->_idx];
          assert(m != NULL || (c->is_Loop() && j == LoopNode::LoopBackControl && iteration == 1) || _phase->C->has_irreducible_loop() || has_never_branch(_phase->C->root()), "expect memory state");
          if (m != NULL) {
            if (m == prev_region && ((c->is_Loop() && j == LoopNode::LoopBackControl) || (prev_region->is_Phi() && prev_region->in(0) == c))) {
              assert(c->is_Loop() && j == LoopNode::LoopBackControl || _phase->C->has_irreducible_loop(), "");
              // continue
            } else if (unique == NULL) {
              unique = m;
            } else if (m == unique) {
              // continue
            } else {
              unique = NodeSentinel;
            }
          }
        }
        assert(unique != NULL, "empty phi???");
        if (unique != NodeSentinel) {
          if (prev_region != NULL && prev_region->is_Phi() && prev_region->in(0) == c) {
            dead_phis.push(prev_region);
          }
          regions.map(c->_idx, unique);
        } else {
          Node* phi = NULL;
          if (prev_region != NULL && prev_region->is_Phi() && prev_region->in(0) == c && prev_region->_idx >= last) {
            phi = prev_region;
            for (uint k = 1; k < c->req(); k++) {
              Node* m = _memory_nodes[c->in(k)->_idx];
              assert(m != NULL, "expect memory state");
              phi->set_req(k, m);
            }
          } else {
            for (DUIterator_Fast jmax, j = c->fast_outs(jmax); j < jmax && phi == NULL; j++) {
              Node* u = c->fast_out(j);
              if (u->is_Phi() && u->bottom_type() == Type::MEMORY &&
                  (u->adr_type() == TypePtr::BOTTOM || _phase->C->get_alias_index(u->adr_type()) == _alias)) {
                phi = u;
                for (uint k = 1; k < c->req() && phi != NULL; k++) {
                  Node* m = _memory_nodes[c->in(k)->_idx];
                  assert(m != NULL, "expect memory state");
                  if (u->in(k) != m) {
                    phi = NULL;
                  }
                }
              }
            }
            if (phi == NULL) {
              phi = new (_phase->C) PhiNode(c, Type::MEMORY, _phase->C->get_adr_type(_alias));
              for (uint k = 1; k < c->req(); k++) {
                Node* m = _memory_nodes[c->in(k)->_idx];
                assert(m != NULL, "expect memory state");
                phi->init_req(k, m);
              }
            }
          }
          assert(phi != NULL, "");
          regions.map(c->_idx, phi);
        }
        Node* current_region = regions[c->_idx];
        if (current_region != prev_region) {
          progress = true;
          if (prev_region == prev_mem) {
            _memory_nodes.map(c->_idx, current_region);
          }
        }
      } else if (prev_mem == NULL || prev_mem->is_Phi() || ctrl_or_self(prev_mem) != c) {
        Node* m = _memory_nodes[_phase->idom(c)->_idx];
        assert(m != NULL, "expect memory state");
        if (m != prev_mem) {
          _memory_nodes.map(c->_idx, m);
          progress = true;
        }
      }
#ifdef ASSERT
      if (trace) { tty->print("X %d", c->_idx);  _memory_nodes[c->_idx]->dump(); }
#endif
    }
  }

  // Replace existing phi with computed memory state for that region
  // if different (could be a new phi or a dominating memory node if
  // that phi was found to be useless).
  while (dead_phis.size() > 0) {
    Node* n = dead_phis.pop();
    n->replace_by(_phase->C->top());
    n->destruct();
  }
  for (int i = rpo_list.size() - 1; i >= 0; i--) {
    Node* c = rpo_list.at(i);
    if (c->is_Region()) {
      Node* n = regions[c->_idx];
      if (n->is_Phi() && n->_idx >= last && n->in(0) == c) {
        _phase->register_new_node(n, c);
      }
    }
  }
  for (int i = rpo_list.size() - 1; i >= 0; i--) {
    Node* c = rpo_list.at(i);
    if (c->is_Region()) {
      Node* n = regions[c->_idx];
      for (DUIterator_Fast imax, i = c->fast_outs(imax); i < imax; i++) {
        Node* u = c->fast_out(i);
        if (u->is_Phi() && u->bottom_type() == Type::MEMORY &&
            u != n) {
          if (u->adr_type() == TypePtr::BOTTOM) {
            fix_memory_uses(u, n, n, c);
          } else if (_phase->C->get_alias_index(u->adr_type()) == _alias) {
            _phase->lazy_replace(u, n);
            --i; --imax;
          }
        }
      }
    }
  }
}

Node* MemoryGraphFixer::get_ctrl(Node* n) const {
  Node* c = _phase->get_ctrl(n);
  if (n->is_Proj() && n->in(0) != NULL && n->in(0)->is_Call()) {
    assert(c == n->in(0), "");
    CallNode* call = c->as_Call();
    CallProjections projs;
    call->extract_projections(&projs, true, false);
    if (projs.catchall_memproj != NULL) {
      if (projs.fallthrough_memproj == n) {
        c = projs.fallthrough_catchproj;
      } else {
        assert(projs.catchall_memproj == n, "");
        c = projs.catchall_catchproj;
      }
    }
  }
  return c;
}

Node* MemoryGraphFixer::ctrl_or_self(Node* n) const {
  if (_phase->has_ctrl(n))
    return get_ctrl(n);
  else {
    assert (n->is_CFG(), "must be a CFG node");
    return n;
  }
}

bool MemoryGraphFixer::mem_is_valid(Node* m, Node* c) const {
  return m != NULL && get_ctrl(m) == c;
}

Node* MemoryGraphFixer::find_mem(Node* ctrl, Node* n) const {
  assert(n == NULL || _phase->ctrl_or_self(n) == ctrl, "");
  Node* mem = _memory_nodes[ctrl->_idx];
  Node* c = ctrl;
  while (!mem_is_valid(mem, c) &&
         (!c->is_CatchProj() || mem == NULL || c->in(0)->in(0)->in(0) != get_ctrl(mem))) {
    c = _phase->idom(c);
    mem = _memory_nodes[c->_idx];
  }
  if (n != NULL && mem_is_valid(mem, c)) {
    while (!ShenandoahBarrierC2Support::is_dominator_same_ctrl(c, mem, n, _phase) && _phase->ctrl_or_self(mem) == ctrl) {
      mem = next_mem(mem, _alias);
    }
    if (mem->is_MergeMem()) {
      mem = mem->as_MergeMem()->memory_at(_alias);
    }
    if (!mem_is_valid(mem, c)) {
      do {
        c = _phase->idom(c);
        mem = _memory_nodes[c->_idx];
      } while (!mem_is_valid(mem, c) &&
               (!c->is_CatchProj() || mem == NULL || c->in(0)->in(0)->in(0) != get_ctrl(mem)));
    }
  }
  assert(mem->bottom_type() == Type::MEMORY, "");
  return mem;
}

bool MemoryGraphFixer::has_mem_phi(Node* region) const {
  for (DUIterator_Fast imax, i = region->fast_outs(imax); i < imax; i++) {
    Node* use = region->fast_out(i);
    if (use->is_Phi() && use->bottom_type() == Type::MEMORY &&
        (_phase->C->get_alias_index(use->adr_type()) == _alias)) {
      return true;
    }
  }
  return false;
}

void MemoryGraphFixer::fix_mem(Node* ctrl, Node* new_ctrl, Node* mem, Node* mem_for_ctrl, Node* new_mem, Unique_Node_List& uses) {
  assert(_phase->ctrl_or_self(new_mem) == new_ctrl, "");
  const bool trace = false;
  DEBUG_ONLY(if (trace) { tty->print("ZZZ control is"); ctrl->dump(); });
  DEBUG_ONLY(if (trace) { tty->print("ZZZ mem is"); mem->dump(); });
  GrowableArray<Node*> phis;
  if (mem_for_ctrl != mem) {
    Node* old = mem_for_ctrl;
    Node* prev = NULL;
    while (old != mem) {
      prev = old;
      if (old->is_Store() || old->is_ClearArray() || old->is_LoadStore()) {
        assert(_alias == Compile::AliasIdxRaw, "");
        old = old->in(MemNode::Memory);
      } else if (old->Opcode() == Op_SCMemProj) {
        assert(_alias == Compile::AliasIdxRaw, "");
        old = old->in(0);
      } else {
        ShouldNotReachHere();
      }
    }
    assert(prev != NULL, "");
    if (new_ctrl != ctrl) {
      _memory_nodes.map(ctrl->_idx, mem);
      _memory_nodes.map(new_ctrl->_idx, mem_for_ctrl);
    }
    uint input = (uint)MemNode::Memory;
    _phase->igvn().replace_input_of(prev, input, new_mem);
  } else {
    uses.clear();
    _memory_nodes.map(new_ctrl->_idx, new_mem);
    uses.push(new_ctrl);
    for(uint next = 0; next < uses.size(); next++ ) {
      Node *n = uses.at(next);
      assert(n->is_CFG(), "");
      DEBUG_ONLY(if (trace) { tty->print("ZZZ ctrl"); n->dump(); });
      for (DUIterator_Fast imax, i = n->fast_outs(imax); i < imax; i++) {
        Node* u = n->fast_out(i);
        if (!u->is_Root() && u->is_CFG() && u != n) {
          Node* m = _memory_nodes[u->_idx];
          if (u->is_Region() &&
              !has_mem_phi(u) &&
              u->unique_ctrl_out()->Opcode() != Op_Halt) {
            DEBUG_ONLY(if (trace) { tty->print("ZZZ region"); u->dump(); });
            DEBUG_ONLY(if (trace && m != NULL) { tty->print("ZZZ mem"); m->dump(); });

            if (!mem_is_valid(m, u) || !m->is_Phi()) {
              bool push = true;
              bool create_phi = true;
              if (_phase->is_dominator(new_ctrl, u)) {
                create_phi = false;
              }
              if (create_phi) {
                Node* phi = new (_phase->C) PhiNode(u, Type::MEMORY, _phase->C->get_adr_type(_alias));
                _phase->register_new_node(phi, u);
                phis.push(phi);
                DEBUG_ONLY(if (trace) { tty->print("ZZZ new phi"); phi->dump(); });
                if (!mem_is_valid(m, u)) {
                  DEBUG_ONLY(if (trace) { tty->print("ZZZ setting mem"); phi->dump(); });
                  _memory_nodes.map(u->_idx, phi);
                } else {
                  DEBUG_ONLY(if (trace) { tty->print("ZZZ NOT setting mem"); m->dump(); });
                  for (;;) {
                    assert(m->is_Mem() || m->is_LoadStore() || m->is_Proj(), "");
                    Node* next = NULL;
                    if (m->is_Proj()) {
                      next = m->in(0);
                    } else {
                      assert(m->is_Mem() || m->is_LoadStore(), "");
                      assert(_alias == Compile::AliasIdxRaw, "");
                      next = m->in(MemNode::Memory);
                    }
                    if (_phase->get_ctrl(next) != u) {
                      break;
                    }
                    if (next->is_MergeMem()) {
                      assert(_phase->get_ctrl(next->as_MergeMem()->memory_at(_alias)) != u, "");
                      break;
                    }
                    if (next->is_Phi()) {
                      assert(next->adr_type() == TypePtr::BOTTOM && next->in(0) == u, "");
                      break;
                    }
                    m = next;
                  }

                  DEBUG_ONLY(if (trace) { tty->print("ZZZ setting to phi"); m->dump(); });
                  assert(m->is_Mem() || m->is_LoadStore(), "");
                  uint input = (uint)MemNode::Memory;
                  _phase->igvn().replace_input_of(m, input, phi);
                  push = false;
                }
              } else {
                DEBUG_ONLY(if (trace) { tty->print("ZZZ skipping region"); u->dump(); });
              }
              if (push) {
                uses.push(u);
              }
            }
          } else if (!mem_is_valid(m, u) &&
                     !(u->Opcode() == Op_CProj && u->in(0)->Opcode() == Op_NeverBranch && u->as_Proj()->_con == 1)) {
            uses.push(u);
          }
        }
      }
    }
    for (int i = 0; i < phis.length(); i++) {
      Node* n = phis.at(i);
      Node* r = n->in(0);
      DEBUG_ONLY(if (trace) { tty->print("ZZZ fixing new phi"); n->dump(); });
      for (uint j = 1; j < n->req(); j++) {
        Node* m = find_mem(r->in(j), NULL);
        _phase->igvn().replace_input_of(n, j, m);
        DEBUG_ONLY(if (trace) { tty->print("ZZZ fixing new phi: %d", j); m->dump(); });
      }
    }
  }
  uint last = _phase->C->unique();
  MergeMemNode* mm = NULL;
  int alias = _alias;
  DEBUG_ONLY(if (trace) { tty->print("ZZZ raw mem is"); mem->dump(); });
  // Process loads first to not miss an anti-dependency: if the memory
  // edge of a store is updated before a load is processed then an
  // anti-dependency may be missed.
  for (DUIterator i = mem->outs(); mem->has_out(i); i++) {
    Node* u = mem->out(i);
    if (u->_idx < last && u->is_Load() && _phase->C->get_alias_index(u->adr_type()) == alias) {
      Node* m = find_mem(_phase->get_ctrl(u), u);
      if (m != mem) {
        DEBUG_ONLY(if (trace) { tty->print("ZZZ setting memory of use"); u->dump(); });
        _phase->igvn().replace_input_of(u, MemNode::Memory, m);
        --i;
      }
    }
  }
  for (DUIterator i = mem->outs(); mem->has_out(i); i++) {
    Node* u = mem->out(i);
    if (u->_idx < last) {
      if (u->is_Mem()) {
        if (_phase->C->get_alias_index(u->adr_type()) == alias) {
          Node* m = find_mem(_phase->get_ctrl(u), u);
          if (m != mem) {
            DEBUG_ONLY(if (trace) { tty->print("ZZZ setting memory of use"); u->dump(); });
            _phase->igvn().replace_input_of(u, MemNode::Memory, m);
            --i;
          }
        }
      } else if (u->is_MergeMem()) {
        MergeMemNode* u_mm = u->as_MergeMem();
        if (u_mm->memory_at(alias) == mem) {
          MergeMemNode* newmm = NULL;
          for (DUIterator_Fast jmax, j = u->fast_outs(jmax); j < jmax; j++) {
            Node* uu = u->fast_out(j);
            assert(!uu->is_MergeMem(), "chain of MergeMems?");
            if (uu->is_Phi()) {
              assert(uu->adr_type() == TypePtr::BOTTOM, "");
              Node* region = uu->in(0);
              int nb = 0;
              for (uint k = 1; k < uu->req(); k++) {
                if (uu->in(k) == u) {
                  Node* m = find_mem(region->in(k), NULL);
                  if (m != mem) {
                    DEBUG_ONLY(if (trace) { tty->print("ZZZ setting memory of phi %d", k); uu->dump(); });
                    newmm = clone_merge_mem(u, mem, m, _phase->ctrl_or_self(m), i);
                    if (newmm != u) {
                      _phase->igvn().replace_input_of(uu, k, newmm);
                      nb++;
                      --jmax;
                    }
                  }
                }
              }
              if (nb > 0) {
                --j;
              }
            } else {
              Node* m = find_mem(_phase->ctrl_or_self(uu), uu);
              if (m != mem) {
                DEBUG_ONLY(if (trace) { tty->print("ZZZ setting memory of use"); uu->dump(); });
                newmm = clone_merge_mem(u, mem, m, _phase->ctrl_or_self(m), i);
                if (newmm != u) {
                  _phase->igvn().replace_input_of(uu, uu->find_edge(u), newmm);
                  --j, --jmax;
                }
              }
            }
          }
        }
      } else if (u->is_Phi()) {
        assert(u->bottom_type() == Type::MEMORY, "what else?");
        if (_phase->C->get_alias_index(u->adr_type()) == alias || u->adr_type() == TypePtr::BOTTOM) {
          Node* region = u->in(0);
          bool replaced = false;
          for (uint j = 1; j < u->req(); j++) {
            if (u->in(j) == mem) {
              Node* m = find_mem(region->in(j), NULL);
              Node* nnew = m;
              if (m != mem) {
                if (u->adr_type() == TypePtr::BOTTOM) {
                  mm = allocate_merge_mem(mem, m, _phase->ctrl_or_self(m));
                  nnew = mm;
                }
                DEBUG_ONLY(if (trace) { tty->print("ZZZ setting memory of phi %d", j); u->dump(); });
                _phase->igvn().replace_input_of(u, j, nnew);
                replaced = true;
              }
            }
          }
          if (replaced) {
            --i;
          }
        }
      } else if ((u->adr_type() == TypePtr::BOTTOM) ||
                 u->adr_type() == NULL) {
        assert(u->adr_type() != NULL ||
               u->Opcode() == Op_Rethrow ||
               u->Opcode() == Op_Return ||
               u->Opcode() == Op_SafePoint ||
               (u->is_CallStaticJava() && u->as_CallStaticJava()->uncommon_trap_request() != 0) ||
               (u->is_CallStaticJava() && u->as_CallStaticJava()->_entry_point == OptoRuntime::rethrow_stub()) ||
               u->Opcode() == Op_CallLeaf, "");
        Node* m = find_mem(_phase->ctrl_or_self(u), u);
        if (m != mem) {
          mm = allocate_merge_mem(mem, m, _phase->get_ctrl(m));
          _phase->igvn().replace_input_of(u, u->find_edge(mem), mm);
          --i;
        }
      } else if (_phase->C->get_alias_index(u->adr_type()) == alias) {
        Node* m = find_mem(_phase->ctrl_or_self(u), u);
        if (m != mem) {
          DEBUG_ONLY(if (trace) { tty->print("ZZZ setting memory of use"); u->dump(); });
          _phase->igvn().replace_input_of(u, u->find_edge(mem), m);
          --i;
        }
      } else if (u->adr_type() != TypePtr::BOTTOM &&
                 _memory_nodes[_phase->ctrl_or_self(u)->_idx] == u) {
        Node* m = find_mem(_phase->ctrl_or_self(u), u);
        assert(m != mem, "");
        // u is on the wrong slice...
        assert(u->is_ClearArray(), "");
        DEBUG_ONLY(if (trace) { tty->print("ZZZ setting memory of use"); u->dump(); });
        _phase->igvn().replace_input_of(u, u->find_edge(mem), m);
        --i;
      }
    }
  }
#ifdef ASSERT
  assert(new_mem->outcnt() > 0, "");
  for (int i = 0; i < phis.length(); i++) {
    Node* n = phis.at(i);
    assert(n->outcnt() > 0, "new phi must have uses now");
  }
#endif
}

MergeMemNode* MemoryGraphFixer::allocate_merge_mem(Node* mem, Node* rep_proj, Node* rep_ctrl) const {
  MergeMemNode* mm = MergeMemNode::make(_phase->C, mem);
  mm->set_memory_at(_alias, rep_proj);
  _phase->register_new_node(mm, rep_ctrl);
  return mm;
}

MergeMemNode* MemoryGraphFixer::clone_merge_mem(Node* u, Node* mem, Node* rep_proj, Node* rep_ctrl, DUIterator& i) const {
  MergeMemNode* newmm = NULL;
  MergeMemNode* u_mm = u->as_MergeMem();
  Node* c = _phase->get_ctrl(u);
  if (_phase->is_dominator(c, rep_ctrl)) {
    c = rep_ctrl;
  } else {
    assert(_phase->is_dominator(rep_ctrl, c), "one must dominate the other");
  }
  if (u->outcnt() == 1) {
    if (u->req() > (uint)_alias && u->in(_alias) == mem) {
      _phase->igvn().replace_input_of(u, _alias, rep_proj);
      --i;
    } else {
      _phase->igvn().rehash_node_delayed(u);
      u_mm->set_memory_at(_alias, rep_proj);
    }
    newmm = u_mm;
    _phase->set_ctrl_and_loop(u, c);
  } else {
    // can't simply clone u and then change one of its input because
    // it adds and then removes an edge which messes with the
    // DUIterator
    newmm = MergeMemNode::make(_phase->C, u_mm->base_memory());
    for (uint j = 0; j < u->req(); j++) {
      if (j < newmm->req()) {
        if (j == (uint)_alias) {
          newmm->set_req(j, rep_proj);
        } else if (newmm->in(j) != u->in(j)) {
          newmm->set_req(j, u->in(j));
        }
      } else if (j == (uint)_alias) {
        newmm->add_req(rep_proj);
      } else {
        newmm->add_req(u->in(j));
      }
    }
    if ((uint)_alias >= u->req()) {
      newmm->set_memory_at(_alias, rep_proj);
    }
    _phase->register_new_node(newmm, c);
  }
  return newmm;
}

bool MemoryGraphFixer::should_process_phi(Node* phi) const {
  if (phi->adr_type() == TypePtr::BOTTOM) {
    Node* region = phi->in(0);
    for (DUIterator_Fast jmax, j = region->fast_outs(jmax); j < jmax; j++) {
      Node* uu = region->fast_out(j);
      if (uu->is_Phi() && uu != phi && uu->bottom_type() == Type::MEMORY && _phase->C->get_alias_index(uu->adr_type()) == _alias) {
        return false;
      }
    }
    return true;
  }
  return _phase->C->get_alias_index(phi->adr_type()) == _alias;
}

void MemoryGraphFixer::fix_memory_uses(Node* mem, Node* replacement, Node* rep_proj, Node* rep_ctrl) const {
  uint last = _phase-> C->unique();
  MergeMemNode* mm = NULL;
  assert(mem->bottom_type() == Type::MEMORY, "");
  for (DUIterator i = mem->outs(); mem->has_out(i); i++) {
    Node* u = mem->out(i);
    if (u != replacement && u->_idx < last) {
      if (u->is_MergeMem()) {
        MergeMemNode* u_mm = u->as_MergeMem();
        if (u_mm->memory_at(_alias) == mem) {
          MergeMemNode* newmm = NULL;
          for (DUIterator_Fast jmax, j = u->fast_outs(jmax); j < jmax; j++) {
            Node* uu = u->fast_out(j);
            assert(!uu->is_MergeMem(), "chain of MergeMems?");
            if (uu->is_Phi()) {
              if (should_process_phi(uu)) {
                Node* region = uu->in(0);
                int nb = 0;
                for (uint k = 1; k < uu->req(); k++) {
                  if (uu->in(k) == u && _phase->is_dominator(rep_ctrl, region->in(k))) {
                    if (newmm == NULL) {
                      newmm = clone_merge_mem(u, mem, rep_proj, rep_ctrl, i);
                    }
                    if (newmm != u) {
                      _phase->igvn().replace_input_of(uu, k, newmm);
                      nb++;
                      --jmax;
                    }
                  }
                }
                if (nb > 0) {
                  --j;
                }
              }
            } else {
              if (rep_ctrl != uu && ShenandoahBarrierC2Support::is_dominator(rep_ctrl, _phase->ctrl_or_self(uu), replacement, uu, _phase)) {
                if (newmm == NULL) {
                  newmm = clone_merge_mem(u, mem, rep_proj, rep_ctrl, i);
                }
                if (newmm != u) {
                  _phase->igvn().replace_input_of(uu, uu->find_edge(u), newmm);
                  --j, --jmax;
                }
              }
            }
          }
        }
      } else if (u->is_Phi()) {
        assert(u->bottom_type() == Type::MEMORY, "what else?");
        Node* region = u->in(0);
        if (should_process_phi(u)) {
          bool replaced = false;
          for (uint j = 1; j < u->req(); j++) {
            if (u->in(j) == mem && _phase->is_dominator(rep_ctrl, region->in(j))) {
              Node* nnew = rep_proj;
              if (u->adr_type() == TypePtr::BOTTOM) {
                if (mm == NULL) {
                  mm = allocate_merge_mem(mem, rep_proj, rep_ctrl);
                }
                nnew = mm;
              }
              _phase->igvn().replace_input_of(u, j, nnew);
              replaced = true;
            }
          }
          if (replaced) {
            --i;
          }

        }
      } else if ((u->adr_type() == TypePtr::BOTTOM) ||
                 u->adr_type() == NULL) {
        assert(u->adr_type() != NULL ||
               u->Opcode() == Op_Rethrow ||
               u->Opcode() == Op_Return ||
               u->Opcode() == Op_SafePoint ||
               u->Opcode() == Op_StoreIConditional ||
               u->Opcode() == Op_StoreLConditional ||
               (u->is_CallStaticJava() && u->as_CallStaticJava()->uncommon_trap_request() != 0) ||
               (u->is_CallStaticJava() && u->as_CallStaticJava()->_entry_point == OptoRuntime::rethrow_stub()) ||
               u->Opcode() == Op_CallLeaf, err_msg("%s", u->Name()));
        if (ShenandoahBarrierC2Support::is_dominator(rep_ctrl, _phase->ctrl_or_self(u), replacement, u, _phase)) {
          if (mm == NULL) {
            mm = allocate_merge_mem(mem, rep_proj, rep_ctrl);
          }
          _phase->igvn().replace_input_of(u, u->find_edge(mem), mm);
          --i;
        }
      } else if (_phase->C->get_alias_index(u->adr_type()) == _alias) {
        if (ShenandoahBarrierC2Support::is_dominator(rep_ctrl, _phase->ctrl_or_self(u), replacement, u, _phase)) {
          _phase->igvn().replace_input_of(u, u->find_edge(mem), rep_proj);
          --i;
        }
      }
    }
  }
}

ShenandoahLoadReferenceBarrierNode::ShenandoahLoadReferenceBarrierNode(Node* ctrl, Node* obj)
: Node(ctrl, obj) {
  Compile::current()->add_shenandoah_barrier(this);
}

const Type* ShenandoahLoadReferenceBarrierNode::bottom_type() const {
  if (in(ValueIn) == NULL || in(ValueIn)->is_top()) {
    return Type::TOP;
  }
  const Type* t = in(ValueIn)->bottom_type();
  if (t == TypePtr::NULL_PTR) {
    return t;
  }
  return t->is_oopptr();
}

const Type* ShenandoahLoadReferenceBarrierNode::Value(PhaseTransform *phase) const {
  // Either input is TOP ==> the result is TOP
  const Type *t2 = phase->type(in(ValueIn));
  if( t2 == Type::TOP ) return Type::TOP;

  if (t2 == TypePtr::NULL_PTR) {
    return t2;
  }

  const Type* type = t2->is_oopptr();
  return type;
}

Node* ShenandoahLoadReferenceBarrierNode::Identity(PhaseTransform *phase) {
  Node* value = in(ValueIn);
  if (!needs_barrier(phase, value)) {
    return value;
  }
  return this;
}

bool ShenandoahLoadReferenceBarrierNode::needs_barrier(PhaseTransform* phase, Node* n) {
  Unique_Node_List visited;
  return needs_barrier_impl(phase, n, visited);
}

bool ShenandoahLoadReferenceBarrierNode::needs_barrier_impl(PhaseTransform* phase, Node* n, Unique_Node_List &visited) {
  if (n == NULL) return false;
  if (visited.member(n)) {
    return false; // Been there.
  }
  visited.push(n);

  if (n->is_Allocate()) {
    // tty->print_cr("optimize barrier on alloc");
    return false;
  }
  if (n->is_Call()) {
    // tty->print_cr("optimize barrier on call");
    return false;
  }

  const Type* type = phase->type(n);
  if (type == Type::TOP) {
    return false;
  }
  if (type->make_ptr()->higher_equal(TypePtr::NULL_PTR)) {
    // tty->print_cr("optimize barrier on null");
    return false;
  }
  // Impl detail: Need to check isa_(narrow)oop before calling to make_oopptr on potentially non-oop types
  // in 8u, otherwise make_oopptr would assert. make_oopptr is fixed later during JDK-8078629.
  if ((type->isa_oopptr() || type->isa_narrowoop()) && type->make_oopptr()->const_oop() != NULL) {
    // tty->print_cr("optimize barrier on constant");
    return false;
  }

  switch (n->Opcode()) {
    case Op_AddP:
      return true; // TODO: Can refine?
    case Op_LoadP:
    case Op_GetAndSetN:
    case Op_GetAndSetP:
      return true;
    case Op_Phi: {
      for (uint i = 1; i < n->req(); i++) {
        if (needs_barrier_impl(phase, n->in(i), visited)) return true;
      }
      return false;
    }
    case Op_CheckCastPP:
    case Op_CastPP:
      return needs_barrier_impl(phase, n->in(1), visited);
    case Op_Proj:
      return needs_barrier_impl(phase, n->in(0), visited);
    case Op_ShenandoahLoadReferenceBarrier:
      // tty->print_cr("optimize barrier on barrier");
      return false;
    case Op_Parm:
      // tty->print_cr("optimize barrier on input arg");
      return false;
    case Op_DecodeN:
    case Op_EncodeP:
      return needs_barrier_impl(phase, n->in(1), visited);
    case Op_LoadN:
      return true;
    case Op_CMoveN:
    case Op_CMoveP:
      return needs_barrier_impl(phase, n->in(2), visited) ||
             needs_barrier_impl(phase, n->in(3), visited);
    case Op_CreateEx:
      return false;
    default:
      break;
  }
#ifdef ASSERT
  tty->print("need barrier on?: ");
  tty->print_cr("ins:");
  n->dump(2);
  tty->print_cr("outs:");
  n->dump(-2);
  ShouldNotReachHere();
#endif
  return true;
}

ShenandoahLoadReferenceBarrierNode::Strength ShenandoahLoadReferenceBarrierNode::get_barrier_strength() {
  Unique_Node_List visited;
  Node_Stack stack(0);
  stack.push(this, 0);

  // Look for strongest strength: go over nodes looking for STRONG ones.
  // Stop once we encountered STRONG. Otherwise, walk until we ran out of nodes,
  // and then the overall strength is NONE.
  Strength strength = NONE;
  while (strength != STRONG && stack.size() > 0) {
    Node* n = stack.node();
    if (visited.member(n)) {
      stack.pop();
      continue;
    }
    visited.push(n);
    bool visit_users = false;
    switch (n->Opcode()) {
      case Op_CallStaticJava:
      case Op_CallDynamicJava:
      case Op_CallLeaf:
      case Op_CallLeafNoFP:
      case Op_CompareAndSwapL:
      case Op_CompareAndSwapI:
      case Op_CompareAndSwapN:
      case Op_CompareAndSwapP:
      case Op_ShenandoahCompareAndSwapN:
      case Op_ShenandoahCompareAndSwapP:
      case Op_GetAndSetL:
      case Op_GetAndSetI:
      case Op_GetAndSetP:
      case Op_GetAndSetN:
      case Op_GetAndAddL:
      case Op_GetAndAddI:
      case Op_FastLock:
      case Op_FastUnlock:
      case Op_Rethrow:
      case Op_Return:
      case Op_StoreB:
      case Op_StoreC:
      case Op_StoreD:
      case Op_StoreF:
      case Op_StoreL:
      case Op_StoreLConditional:
      case Op_StoreI:
      case Op_StoreIConditional:
      case Op_StoreN:
      case Op_StoreP:
      case Op_StoreVector:
      case Op_EncodeP:
      case Op_CastP2X:
      case Op_SafePoint:
      case Op_EncodeISOArray:
      case Op_AryEq:
      case Op_StrEquals:
      case Op_StrComp:
      case Op_StrIndexOf:
        // Known to require barriers
        strength = STRONG;
        break;
      case Op_CmpP: {
        if (n->in(1)->bottom_type()->higher_equal(TypePtr::NULL_PTR) ||
            n->in(2)->bottom_type()->higher_equal(TypePtr::NULL_PTR)) {
          // One of the sides is known null, no need for barrier.
        } else {
          strength = STRONG;
        }
        break;
      }
      case Op_LoadB:
      case Op_LoadUB:
      case Op_LoadUS:
      case Op_LoadD:
      case Op_LoadF:
      case Op_LoadL:
      case Op_LoadI:
      case Op_LoadS:
      case Op_LoadN:
      case Op_LoadP:
      case Op_LoadVector: {
        const TypePtr* adr_type = n->adr_type();
        int alias_idx = Compile::current()->get_alias_index(adr_type);
        Compile::AliasType* alias_type = Compile::current()->alias_type(alias_idx);
        ciField* field = alias_type->field();
        bool is_static = field != NULL && field->is_static();
        bool is_final = field != NULL && field->is_final();

        if (ShenandoahOptimizeStaticFinals && is_static && is_final) {
          // Loading the constant does not require barriers: it should be handled
          // as part of GC roots already.
        } else {
          strength = STRONG;
        }
        break;
      }
      case Op_Conv2B:
      case Op_LoadRange:
      case Op_LoadKlass:
      case Op_LoadNKlass:
        // Do not require barriers
        break;
      case Op_AddP:
      case Op_CheckCastPP:
      case Op_CastPP:
      case Op_CMoveP:
      case Op_Phi:
      case Op_ShenandoahLoadReferenceBarrier:
        // Whether or not these need the barriers depends on their users
        visit_users = true;
        break;
      default: {
#ifdef ASSERT
        fatal(err_msg("Unknown node in get_barrier_strength: %s", NodeClassNames[n->Opcode()]));
#else
        // Default to strong: better to have excess barriers, rather than miss some.
        strength = STRONG;
#endif
      }
    }

    stack.pop();
    if (visit_users) {
      for (DUIterator_Fast imax, i = n->fast_outs(imax); i < imax; i++) {
        Node* user = n->fast_out(i);
        if (user != NULL) {
          stack.push(user, 0);
        }
      }
    }
  }
  return strength;
}

CallStaticJavaNode* ShenandoahLoadReferenceBarrierNode::pin_and_expand_null_check(PhaseIterGVN& igvn) {
  Node* val = in(ValueIn);

  const Type* val_t = igvn.type(val);

  if (val_t->meet(TypePtr::NULL_PTR) != val_t &&
      val->Opcode() == Op_CastPP &&
      val->in(0) != NULL &&
      val->in(0)->Opcode() == Op_IfTrue &&
      val->in(0)->as_Proj()->is_uncommon_trap_if_pattern(Deoptimization::Reason_none) &&
      val->in(0)->in(0)->is_If() &&
      val->in(0)->in(0)->in(1)->Opcode() == Op_Bool &&
      val->in(0)->in(0)->in(1)->as_Bool()->_test._test == BoolTest::ne &&
      val->in(0)->in(0)->in(1)->in(1)->Opcode() == Op_CmpP &&
      val->in(0)->in(0)->in(1)->in(1)->in(1) == val->in(1) &&
      val->in(0)->in(0)->in(1)->in(1)->in(2)->bottom_type() == TypePtr::NULL_PTR) {
    assert(val->in(0)->in(0)->in(1)->in(1)->in(1) == val->in(1), "");
    CallStaticJavaNode* unc = val->in(0)->as_Proj()->is_uncommon_trap_if_pattern(Deoptimization::Reason_none);
    return unc;
  }
  return NULL;
}