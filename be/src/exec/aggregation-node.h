// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.


#ifndef IMPALA_EXEC_AGGREGATION_NODE_H
#define IMPALA_EXEC_AGGREGATION_NODE_H

#include <boost/scoped_ptr.hpp>

#include "exec/exec-node.h"
#include "exec/old-hash-table.h"
#include "exprs/agg-fn.h"
#include "runtime/descriptors.h"  // for TupleId
#include "runtime/mem-pool.h"
#include "runtime/string-value.h"

namespace llvm {
  class Function;
}

namespace impala {

class AggFnEvaluator;
class LlvmCodeGen;
class RowBatch;
class RuntimeState;
struct StringValue;
class Tuple;
class TupleDescriptor;
class SlotDescriptor;

/// Node for in-memory hash aggregation.
/// The node creates a hash set of aggregation intermediate tuples, which
/// contain slots for all grouping and aggregation exprs (the grouping
/// slots precede the aggregation expr slots in the output tuple descriptor).
//
/// TODO: codegen cross-compiled UDAs and get rid of handcrafted IR.
/// TODO: investigate high compile time for wide tables
class AggregationNode : public ExecNode {
 public:
  AggregationNode(ObjectPool* pool, const TPlanNode& tnode, const DescriptorTbl& descs);

  virtual Status Init(const TPlanNode& tnode, RuntimeState* state);
  virtual Status Prepare(RuntimeState* state);
  virtual void Codegen(RuntimeState* state);
  virtual Status Open(RuntimeState* state);
  virtual Status GetNext(RuntimeState* state, RowBatch* row_batch, bool* eos);
  virtual Status Reset(RuntimeState* state);
  virtual void Close(RuntimeState* state);

  static const char* LLVM_CLASS_NAME;

 protected:
  virtual Status QueryMaintenance(RuntimeState* state);
  virtual void DebugString(int indentation_level, std::stringstream* out) const;

 private:
  boost::scoped_ptr<OldHashTable> hash_tbl_;
  OldHashTable::Iterator output_iterator_;

  /// The list of all aggregate operations for this exec node.
  std::vector<AggFn*> agg_fns_;
  std::vector<AggFnEvaluator*> agg_fn_evals_;

  /// Backing MemPools of 'agg_fn_evals_'.
  boost::scoped_ptr<MemPool> agg_fn_pool_;

  /// Group-by exprs used to evaluate input rows.
  std::vector<ScalarExpr*> grouping_exprs_;

  /// Exprs used to insert constructed aggregation tuple into the hash table.
  /// All the exprs are simply SlotRefs for the intermediate tuple.
  std::vector<ScalarExpr*> build_exprs_;

  /// Tuple into which Update()/Merge()/Serialize() results are stored.
  TupleId intermediate_tuple_id_;
  TupleDescriptor* intermediate_tuple_desc_;

  /// Construct a new row desc for preparing the build exprs because neither the child's
  /// nor this node's output row desc may contain the intermediate tuple, e.g.,
  /// in a single-node plan with an intermediate tuple different from the output tuple.
  /// Lives in the query state's obj_pool.
  RowDescriptor* intermediate_row_desc_;

  /// Tuple into which Finalize() results are stored. Possibly the same as
  /// the intermediate tuple.
  TupleId output_tuple_id_;
  TupleDescriptor* output_tuple_desc_;

  /// Intermediate result of aggregation w/o GROUP BY.
  /// Note: can be NULL even if there is no grouping if the result tuple is 0 width
  Tuple* singleton_intermediate_tuple_;

  boost::scoped_ptr<MemPool> tuple_pool_;

  /// IR for process row batch.  NULL if codegen is disabled.
  llvm::Function* codegen_process_row_batch_fn_;

  typedef void (*ProcessRowBatchFn)(AggregationNode*, RowBatch*);
  /// Jitted ProcessRowBatch function pointer.  Null if codegen is disabled.
  ProcessRowBatchFn process_row_batch_fn_;

  /// Certain aggregates require a finalize step, which is the final step of the
  /// aggregate after consuming all input rows. The finalize step converts the aggregate
  /// value into its final form. This is true if this node contains aggregate that requires
  /// a finalize step.
  bool needs_finalize_;

  /// Time spent processing the child rows
  RuntimeProfile::Counter* build_timer_;
  /// Time spent returning the aggregated rows
  RuntimeProfile::Counter* get_results_timer_;
  /// Num buckets in hash table
  RuntimeProfile::Counter* hash_table_buckets_counter_;
  /// Load factor in hash table
  RuntimeProfile::Counter* hash_table_load_factor_counter_;

  /// Constructs a new aggregation intermediate tuple (allocated from tuple_pool_),
  /// initialized to grouping values computed over 'current_row_'.
  /// Aggregation expr slots are set to their initial values.
  Tuple* ConstructIntermediateTuple();

  /// Updates the aggregation intermediate tuple 'tuple' with aggregation values
  /// computed over 'row'. This function is replaced by codegen.
  void UpdateTuple(Tuple* tuple, TupleRow* row);

  /// Called on the intermediate tuple of each group after all input rows have been
  /// consumed and aggregated. Computes the final aggregate values to be returned in
  /// GetNext() using the agg fn evaluators' Serialize() or Finalize().
  /// For the Finalize() case if the output tuple is different from the intermediate
  /// tuple, then a new tuple is allocated from 'pool' to hold the final result.
  /// Returns the tuple holding the final aggregate values.
  Tuple* FinalizeTuple(Tuple* tuple, MemPool* pool);

  /// Cross-compiled accessor for 'agg_fn_evals_'. Used by the codegen'ed code.
  AggFnEvaluator* const* IR_ALWAYS_INLINE agg_fn_evals() const;

  /// Do the aggregation for all tuple rows in the batch
  void ProcessRowBatchNoGrouping(RowBatch* batch);
  void ProcessRowBatchWithGrouping(RowBatch* batch);

  /// Codegen the process row batch loop.  The loop has already been compiled to
  /// IR and loaded into the codegen object.  UpdateAggTuple has also been
  /// codegen'd to IR.  This function will modify the loop subsituting the
  /// UpdateAggTuple function call with the (inlined) codegen'd 'update_tuple_fn'.
  llvm::Function* CodegenProcessRowBatch(LlvmCodeGen* codegen,
      llvm::Function* update_tuple_fn);

  /// Codegen for updating aggregate_exprs at agg_fn_idx. Returns NULL if unsuccessful.
  /// agg_fn_idx is the idx into agg_fns_ (does not include grouping exprs).
  llvm::Function* CodegenUpdateSlot(LlvmCodeGen* codegen, int agg_fn_idx,
      SlotDescriptor* slot_desc);

  /// Codegen UpdateTuple(). Returns NULL if codegen is unsuccessful.
  llvm::Function* CodegenUpdateTuple(LlvmCodeGen* codegen);
};

}

#endif
