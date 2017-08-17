/* Copyright 2017 Stanford University, NVIDIA Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include "legion.h"
#include "legion_ops.h"
#include "legion_spy.h"
#include "legion_trace.h"
#include "legion_tasks.h"
#include "legion_context.h"
#include "logger_message_descriptor.h"

namespace Legion {
  namespace Internal {

    LEGION_EXTERN_LOGGER_DECLARATIONS

    /////////////////////////////////////////////////////////////
    // LegionTrace 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    LegionTrace::LegionTrace(TaskContext *c)
      : ctx(c), physical_trace(NULL)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    LegionTrace::~LegionTrace(void)
    //--------------------------------------------------------------------------
    {
      if (physical_trace != NULL)
        delete physical_trace;
    }

    //--------------------------------------------------------------------------
    void LegionTrace::replay_aliased_children(
                             std::vector<RegionTreePath> &privilege_paths) const
    //--------------------------------------------------------------------------
    {
      unsigned index = operations.size() - 1;
      std::map<unsigned,LegionVector<AliasChildren>::aligned>::const_iterator
        finder = aliased_children.find(index);
      if (finder == aliased_children.end())
        return;
      for (LegionVector<AliasChildren>::aligned::const_iterator it = 
            finder->second.begin(); it != finder->second.end(); it++)
      {
#ifdef DEBUG_LEGION
        assert(it->req_index < privilege_paths.size());
#endif
        privilege_paths[it->req_index].record_aliased_children(it->depth,
                                                               it->mask);
      }
    }

    //--------------------------------------------------------------------------
    void LegionTrace::end_trace_execution(FenceOp *op)
    //--------------------------------------------------------------------------
    {
      // Register for this fence on every one of the operations in
      // the trace and then clear out the operations data structure
      for (unsigned idx = 0; idx < operations.size(); idx++)
      {
        const std::pair<Operation*,GenerationID> &target = operations[idx];
        op->register_dependence(target.first, target.second);
#ifdef LEGION_SPY
        for (unsigned req_idx = 0; req_idx < num_regions[idx]; req_idx++)
        {
          LegionSpy::log_mapping_dependence(
              op->get_context()->get_unique_id(), current_uids[idx], req_idx,
              op->get_unique_op_id(), 0, TRUE_DEPENDENCE);
        }
#endif
        // Remove any mapping references that we hold
        target.first->remove_mapping_reference(target.second);
      }
      operations.clear();
#ifdef LEGION_SPY
      current_uids.clear();
      num_regions.clear();
#endif
    }

    /////////////////////////////////////////////////////////////
    // StaticTrace
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    StaticTrace::StaticTrace(TaskContext *c,const std::set<RegionTreeID> *trees)
      : LegionTrace(c)
    //--------------------------------------------------------------------------
    {
      if (trees != NULL)
        application_trees.insert(trees->begin(), trees->end());
    }
    
    //--------------------------------------------------------------------------
    StaticTrace::StaticTrace(const StaticTrace &rhs)
      : LegionTrace(NULL)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    StaticTrace::~StaticTrace(void)
    //--------------------------------------------------------------------------
    {
      // Remove our mapping references and then clear the operations
      for (std::vector<std::pair<Operation*,GenerationID> >::const_iterator it =
            operations.begin(); it != operations.end(); it++)
        it->first->remove_mapping_reference(it->second);
      operations.clear();
    }

    //--------------------------------------------------------------------------
    StaticTrace& StaticTrace::operator=(const StaticTrace &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    bool StaticTrace::is_fixed(void) const
    //--------------------------------------------------------------------------
    {
      // Static traces are always fixed
      return true;
    }

    //--------------------------------------------------------------------------
    bool StaticTrace::handles_region_tree(RegionTreeID tid) const
    //--------------------------------------------------------------------------
    {
      if (application_trees.empty())
        return true;
      return (application_trees.find(tid) != application_trees.end());
    }

    //--------------------------------------------------------------------------
    void StaticTrace::record_static_dependences(Operation *op,
                               const std::vector<StaticDependence> *dependences)
    //--------------------------------------------------------------------------
    {
      // Internal operations get to skip this
      if (op->is_internal_op())
        return;
      // All other operations have to add something to the list
      if (dependences == NULL)
        static_dependences.resize(static_dependences.size() + 1);
      else // Add it to the list of static dependences
        static_dependences.push_back(*dependences);
    }

    //--------------------------------------------------------------------------
    void StaticTrace::register_operation(Operation *op, GenerationID gen)
    //--------------------------------------------------------------------------
    {
      std::pair<Operation*,GenerationID> key(op,gen);
      const unsigned index = operations.size();
      if (op->is_memoizing())
      {
        if (physical_trace == NULL)
        {
          if (index != 0)
          {
            MessageDescriptor INCOMPLETE_PHYSICAL_TRACING(3801, "undefined");
            log_run.error(INCOMPLETE_PHYSICAL_TRACING.id(),
                "Invalid memoization request. A trace cannot be partially "
                "memoized. Please change the mapper to request memoization "
                "for all the tasks in your trace");
#ifdef DEBUG_LEGION
            assert(false);
#endif
            exit(ERROR_INCOMPLETE_PHYSICAL_TRACING);
          }
          physical_trace = new PhysicalTrace();
        }
        op->set_trace_local_id(index);
      }
      else if (!op->is_internal_op() && physical_trace != NULL)
      {
        MessageDescriptor INCOMPLETE_PHYSICAL_TRACING(3802, "undefined");
        log_run.error(INCOMPLETE_PHYSICAL_TRACING.id(),
            "Invalid memoization request. A trace cannot be partially "
            "memoized. Please change the mapper to request memoization "
            "for all the tasks in your trace");
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_INCOMPLETE_PHYSICAL_TRACING);
      }

      if (!op->is_internal_op())
      {
        const LegionVector<DependenceRecord>::aligned &deps = 
          translate_dependence_records(op, index); 
        operations.push_back(key);
#ifdef LEGION_SPY
        current_uids.push_back(op->get_unique_op_id());
        num_regions.push_back(op->get_region_count());
#endif
        // Add a mapping reference since people will be 
        // registering dependences
        op->add_mapping_reference(gen);  
        // Then compute all the dependences on this operation from
        // our previous recording of the trace
        for (LegionVector<DependenceRecord>::aligned::const_iterator it = 
              deps.begin(); it != deps.end(); it++)
        {
#ifdef DEBUG_LEGION
          assert((it->operation_idx >= 0) &&
                 ((size_t)it->operation_idx < operations.size()));
#endif
          const std::pair<Operation*,GenerationID> &target = 
                                                operations[it->operation_idx];

          if ((it->prev_idx == -1) || (it->next_idx == -1))
          {
            op->register_dependence(target.first, target.second);
#ifdef LEGION_SPY
            LegionSpy::log_mapping_dependence(
                op->get_context()->get_unique_id(),
                current_uids[it->operation_idx], 
                (it->prev_idx == -1) ? 0 : it->prev_idx,
                op->get_unique_op_id(), 
                (it->next_idx == -1) ? 0 : it->next_idx, TRUE_DEPENDENCE);
#endif
          }
          else
          {
            op->register_region_dependence(it->next_idx, target.first,
                                           target.second, it->prev_idx,
                                           it->dtype, it->validates,
                                           it->dependent_mask);
#ifdef LEGION_SPY
            LegionSpy::log_mapping_dependence(
                op->get_context()->get_unique_id(),
                current_uids[it->operation_idx], it->prev_idx,
                op->get_unique_op_id(), it->next_idx, it->dtype);
#endif
          }
        }
      }
      else
      {
        // We already added our creator to the list of operations
        // so the set of dependences is index-1
#ifdef DEBUG_LEGION
        assert(index > 0);
#endif
        const LegionVector<DependenceRecord>::aligned &deps = 
          translate_dependence_records(operations[index-1].first, index-1);
        // Special case for internal operations
        // Internal operations need to register transitive dependences
        // on all the other operations with which it interferes.
        // We can get this from the set of operations on which the
        // operation we are currently performing dependence analysis
        // has dependences.
        InternalOp *internal_op = static_cast<InternalOp*>(op);
#ifdef DEBUG_LEGION
        assert(internal_op == dynamic_cast<InternalOp*>(op));
#endif
        int internal_index = internal_op->get_internal_index();
        for (LegionVector<DependenceRecord>::aligned::const_iterator it = 
              deps.begin(); it != deps.end(); it++)
        {
          // We only record dependences for this internal operation on
          // the indexes for which this internal operation is being done
          if (internal_index != it->next_idx)
            continue;
#ifdef DEBUG_LEGION
          assert((it->operation_idx >= 0) &&
                 ((size_t)it->operation_idx < operations.size()));
#endif
          const std::pair<Operation*,GenerationID> &target = 
                                                operations[it->operation_idx];
          // If this is the case we can do the normal registration
          if ((it->prev_idx == -1) || (it->next_idx == -1))
          {
            internal_op->register_dependence(target.first, target.second);
#ifdef LEGION_SPY
            LegionSpy::log_mapping_dependence(
                op->get_context()->get_unique_id(),
                current_uids[it->operation_idx], 
                (it->prev_idx == -1) ? 0 : it->prev_idx,
                op->get_unique_op_id(), 
                (it->next_idx == -1) ? 0 : it->next_idx, TRUE_DEPENDENCE);
#endif
          }
          else
          {
            internal_op->record_trace_dependence(target.first, target.second,
                                               it->prev_idx, it->next_idx,
                                               it->dtype, it->dependent_mask);
#ifdef LEGION_SPY
            LegionSpy::log_mapping_dependence(
                internal_op->get_context()->get_unique_id(),
                current_uids[it->operation_idx], it->prev_idx,
                internal_op->get_unique_op_id(), 0, it->dtype);
#endif
          }
        }
      }
    }

    //--------------------------------------------------------------------------
    void StaticTrace::record_dependence(
                                     Operation *target, GenerationID target_gen,
                                     Operation *source, GenerationID source_gen)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }
    
    //--------------------------------------------------------------------------
    void StaticTrace::record_region_dependence(
                                    Operation *target, GenerationID target_gen,
                                    Operation *source, GenerationID source_gen,
                                    unsigned target_idx, unsigned source_idx,
                                    DependenceType dtype, bool validates,
                                    const FieldMask &dependent_mask)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    void StaticTrace::record_aliased_children(unsigned req_index,unsigned depth,
                                              const FieldMask &aliase_mask)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    const LegionVector<LegionTrace::DependenceRecord>::aligned& 
        StaticTrace::translate_dependence_records(Operation *op, unsigned index)
    //--------------------------------------------------------------------------
    {
      // If we already translated it then we are done
      if (index < translated_deps.size())
        return translated_deps[index];
      const unsigned start_idx = translated_deps.size();
      translated_deps.resize(index+1);
      RegionTreeForest *forest = ctx->runtime->forest;
      for (unsigned op_idx = start_idx; op_idx <= index; op_idx++)
      {
        const std::vector<StaticDependence> &static_deps = 
          static_dependences[op_idx];
        LegionVector<DependenceRecord>::aligned &translation = 
          translated_deps[op_idx];
        for (std::vector<StaticDependence>::const_iterator it = 
              static_deps.begin(); it != static_deps.end(); it++)
        {
          // Convert the previous offset into an absoluate offset    
          // If the previous offset is larger than the index then 
          // this dependence doesn't matter
          if (it->previous_offset > index)
            continue;
          // Compute the field mask by getting the parent region requirement
          unsigned parent_index = op->find_parent_index(it->current_req_index);
          FieldSpace field_space =  
            ctx->find_logical_region(parent_index).get_field_space();
          const FieldMask dependence_mask = 
            forest->get_node(field_space)->get_field_mask(it->dependent_fields);
          translation.push_back(DependenceRecord(index - it->previous_offset, 
                it->previous_req_index, it->current_req_index,
                it->validates, it->dependence_type, dependence_mask));
        }
      }
      return translated_deps[index];
    }

    /////////////////////////////////////////////////////////////
    // DynamicTrace 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    DynamicTrace::DynamicTrace(TraceID t, TaskContext *c)
      : LegionTrace(c), tid(t), fixed(false), tracing(true)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    DynamicTrace::DynamicTrace(const DynamicTrace &rhs)
      : LegionTrace(NULL), tid(0)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    DynamicTrace::~DynamicTrace(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    DynamicTrace& DynamicTrace::operator=(const DynamicTrace &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    void DynamicTrace::fix_trace(void)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!fixed);
#endif
      fixed = true;
    }

    //--------------------------------------------------------------------------
    void DynamicTrace::end_trace_capture(void)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(tracing);
#endif
      operations.clear();
      op_map.clear();
      internal_dependences.clear();
      tracing = false;
#ifdef LEGION_SPY
      current_uids.clear();
      num_regions.clear();
#endif
    } 

    //--------------------------------------------------------------------------
    bool DynamicTrace::handles_region_tree(RegionTreeID tid) const
    //--------------------------------------------------------------------------
    {
      // Always handles all of them
      return true;
    }

    //--------------------------------------------------------------------------
    void DynamicTrace::record_static_dependences(Operation *op,
                               const std::vector<StaticDependence> *dependences)
    //--------------------------------------------------------------------------
    {
      // Nothing to do
    }

    //--------------------------------------------------------------------------
    void DynamicTrace::register_operation(Operation *op, GenerationID gen)
    //--------------------------------------------------------------------------
    {
      std::pair<Operation*,GenerationID> key(op,gen);
      const unsigned index = operations.size();
      if (op->is_memoizing())
      {
        if (physical_trace == NULL)
        {
          if (index != 0)
          {
            MessageDescriptor INCOMPLETE_PHYSICAL_TRACING(3801, "undefined");
            log_run.error(INCOMPLETE_PHYSICAL_TRACING.id(),
                "Invalid memoization request. A trace cannot be partially "
                "memoized. Please change the mapper to request memoization "
                "for all the tasks in your trace");
#ifdef DEBUG_LEGION
            assert(false);
#endif
            exit(ERROR_INCOMPLETE_PHYSICAL_TRACING);
          }
          physical_trace = new PhysicalTrace();
        }
        op->set_trace_local_id(index);
      }
      else if (!op->is_internal_op() && physical_trace != NULL)
      {
        MessageDescriptor INCOMPLETE_PHYSICAL_TRACING(3802, "undefined");
        log_run.error(INCOMPLETE_PHYSICAL_TRACING.id(),
            "Invalid memoization request. A trace cannot be partially "
            "memoized. Please change the mapper to request memoization "
            "for all the tasks in your trace");
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_INCOMPLETE_PHYSICAL_TRACING);
      }

      // Only need to save this in the map if we are not done tracing
      if (tracing)
      {
        // This is the normal case
        if (!op->is_internal_op())
        {
          operations.push_back(key);
          op_map[key] = index;
          // Add a new vector for storing dependences onto the back
          dependences.push_back(LegionVector<DependenceRecord>::aligned());
          // Record meta-data about the trace for verifying that
          // it is being replayed correctly
          op_info.push_back(OperationInfo(op));
        }
        else // Otherwise, track internal operations separately
        {
          std::pair<InternalOp*,GenerationID> 
            local_key(static_cast<InternalOp*>(op),gen);
          internal_dependences[local_key] = 
            LegionVector<DependenceRecord>::aligned();
        }
      }
      else
      {
        if (!op->is_internal_op())
        {
          // Check for exceeding the trace size
          if (index >= dependences.size())
          {
            MessageDescriptor TRACE_VIOLATION_RECORDED(1600, "undefined");
            log_run.error(TRACE_VIOLATION_RECORDED.id(),
                          "Trace violation! Recorded %zd operations in trace "
                          "%d in task %s (UID %lld) but %d operations have "
                          "now been issued!", dependences.size(), tid,
                          ctx->get_task_name(), ctx->get_unique_id(), index+1);
#ifdef DEBUG_LEGION
            assert(false);
#endif
            exit(ERROR_TRACE_VIOLATION);
          }
          // Check to see if the meta-data alignes
          const OperationInfo &info = op_info[index];
          // Check that they are the same kind of operation
          if (info.kind != op->get_operation_kind())
          {
            MessageDescriptor TRACE_VIOLATION_OPERATION(1601, "undefined");
            log_run.error(TRACE_VIOLATION_OPERATION.id(),
                          "Trace violation! Operation at index %d of trace %d "
                          "in task %s (UID %lld) was recorded as having type "
                          "%s but instead has type %s in replay.",
                          index, tid, ctx->get_task_name(),ctx->get_unique_id(),
                          Operation::get_string_rep(info.kind),
                          Operation::get_string_rep(op->get_operation_kind()));
#ifdef DEBUG_LEGION
            assert(false);
#endif
            exit(ERROR_TRACE_VIOLATION);
          }
          // Check that they have the same number of region requirements
          if (info.count != op->get_region_count())
          {
            MessageDescriptor TRACE_VIOLATION_OPERATION2(1602, "undefined");
            log_run.error(TRACE_VIOLATION_OPERATION2.id(),
                          "Trace violation! Operation at index %d of trace %d "
                          "in task %s (UID %lld) was recorded as having %d "
                          "regions, but instead has %zd regions in replay.",
                          index, tid, ctx->get_task_name(),
                          ctx->get_unique_id(), info.count,
                          op->get_region_count());
#ifdef DEBUG_LEGION
            assert(false);
#endif
            exit(ERROR_TRACE_VIOLATION);
          }
          // If we make it here, everything is good
          const LegionVector<DependenceRecord>::aligned &deps = 
                                                          dependences[index];
          operations.push_back(key);
#ifdef LEGION_SPY
          current_uids.push_back(op->get_unique_op_id());
          num_regions.push_back(op->get_region_count());
#endif
          // Add a mapping reference since people will be 
          // registering dependences
          op->add_mapping_reference(gen);  
          // Then compute all the dependences on this operation from
          // our previous recording of the trace
          for (LegionVector<DependenceRecord>::aligned::const_iterator it = 
                deps.begin(); it != deps.end(); it++)
          {
#ifdef DEBUG_LEGION
            assert((it->operation_idx >= 0) &&
                   ((size_t)it->operation_idx < operations.size()));
#endif
            const std::pair<Operation*,GenerationID> &target = 
                                                  operations[it->operation_idx];

            if ((it->prev_idx == -1) || (it->next_idx == -1))
            {
              op->register_dependence(target.first, target.second);
#ifdef LEGION_SPY
              LegionSpy::log_mapping_dependence(
                  op->get_context()->get_unique_id(),
                  current_uids[it->operation_idx], 
                  (it->prev_idx == -1) ? 0 : it->prev_idx,
                  op->get_unique_op_id(), 
                  (it->next_idx == -1) ? 0 : it->next_idx, TRUE_DEPENDENCE);
#endif
            }
            else
            {
              op->register_region_dependence(it->next_idx, target.first,
                                             target.second, it->prev_idx,
                                             it->dtype, it->validates,
                                             it->dependent_mask);
#ifdef LEGION_SPY
              LegionSpy::log_mapping_dependence(
                  op->get_context()->get_unique_id(),
                  current_uids[it->operation_idx], it->prev_idx,
                  op->get_unique_op_id(), it->next_idx, it->dtype);
#endif
            }
          }
        }
        else
        {
          // We already added our creator to the list of operations
          // so the set of dependences is index-1
#ifdef DEBUG_LEGION
          assert(index > 0);
#endif
          const LegionVector<DependenceRecord>::aligned &deps = 
                                                        dependences[index-1];
          // Special case for internal operations
          // Internal operations need to register transitive dependences
          // on all the other operations with which it interferes.
          // We can get this from the set of operations on which the
          // operation we are currently performing dependence analysis
          // has dependences.
          InternalOp *internal_op = static_cast<InternalOp*>(op);
#ifdef DEBUG_LEGION
          assert(internal_op == dynamic_cast<InternalOp*>(op));
#endif
          int internal_index = internal_op->get_internal_index();
          for (LegionVector<DependenceRecord>::aligned::const_iterator it = 
                deps.begin(); it != deps.end(); it++)
          {
            // We only record dependences for this internal operation on
            // the indexes for which this internal operation is being done
            if (internal_index != it->next_idx)
              continue;
#ifdef DEBUG_LEGION
            assert((it->operation_idx >= 0) &&
                   ((size_t)it->operation_idx < operations.size()));
#endif
            const std::pair<Operation*,GenerationID> &target = 
                                                  operations[it->operation_idx];
            // If this is the case we can do the normal registration
            if ((it->prev_idx == -1) || (it->next_idx == -1))
            {
              internal_op->register_dependence(target.first, target.second);
#ifdef LEGION_SPY
              LegionSpy::log_mapping_dependence(
                  op->get_context()->get_unique_id(),
                  current_uids[it->operation_idx], 
                  (it->prev_idx == -1) ? 0 : it->prev_idx,
                  op->get_unique_op_id(), 
                  (it->next_idx == -1) ? 0 : it->next_idx, TRUE_DEPENDENCE);
#endif
            }
            else
            {
              internal_op->record_trace_dependence(target.first, target.second,
                                                 it->prev_idx, it->next_idx,
                                                 it->dtype, it->dependent_mask);
#ifdef LEGION_SPY
              LegionSpy::log_mapping_dependence(
                  internal_op->get_context()->get_unique_id(),
                  current_uids[it->operation_idx], it->prev_idx,
                  internal_op->get_unique_op_id(), 0, it->dtype);
#endif
            }
          }
        }
      }
    }

    //--------------------------------------------------------------------------
    void DynamicTrace::record_dependence(Operation *target,GenerationID tar_gen,
                                         Operation *source,GenerationID src_gen)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(tracing);
      if (!source->is_internal_op())
      {
        assert(operations.back().first == source);
        assert(operations.back().second == src_gen);
      }
#endif
      std::pair<Operation*,GenerationID> target_key(target, tar_gen);
      std::map<std::pair<Operation*,GenerationID>,unsigned>::const_iterator
        finder = op_map.find(target_key);
      // We only need to record it if it falls within our trace
      if (finder != op_map.end())
      {
        // Two cases here
        if (!source->is_internal_op())
        {
          // Normal case
          insert_dependence(DependenceRecord(finder->second));
        }
        else
        {
          // Otherwise this is an internal op so record it special
          // Don't record dependences on our creator
          if (target_key != operations.back())
          {
            std::pair<InternalOp*,GenerationID> 
              src_key(static_cast<InternalOp*>(source), src_gen);
#ifdef DEBUG_LEGION
            assert(internal_dependences.find(src_key) != 
                   internal_dependences.end());
#endif
            insert_dependence(src_key, DependenceRecord(finder->second));
          }
        }
      }
      else if (target->is_internal_op())
      {
        // They shouldn't both be internal operations, if they are, then
        // they should be going through the other path that tracks
        // dependences based on specific region requirements
#ifdef DEBUG_LEGION
        assert(!source->is_internal_op());
#endif
        // First check to see if the internal op is one of ours
        std::pair<InternalOp*,GenerationID> 
          local_key(static_cast<InternalOp*>(target),tar_gen);
        std::map<std::pair<InternalOp*,GenerationID>,
                LegionVector<DependenceRecord>::aligned>::const_iterator
          internal_finder = internal_dependences.find(local_key);
        if (internal_finder != internal_dependences.end())
        {
          const LegionVector<DependenceRecord>::aligned &internal_deps = 
                                                        internal_finder->second;
          for (LegionVector<DependenceRecord>::aligned::const_iterator it = 
                internal_deps.begin(); it != internal_deps.end(); it++)
            insert_dependence(DependenceRecord(it->operation_idx)); 
        }
      }
    }

    //--------------------------------------------------------------------------
    void DynamicTrace::record_region_dependence(Operation *target, 
                                                GenerationID tar_gen,
                                                Operation *source, 
                                                GenerationID src_gen,
                                                unsigned target_idx, 
                                                unsigned source_idx,
                                                DependenceType dtype,
                                                bool validates,
                                                const FieldMask &dep_mask)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(tracing);
      if (!source->is_internal_op())
      {
        assert(operations.back().first == source);
        assert(operations.back().second == src_gen);
      }
#endif
      std::pair<Operation*,GenerationID> target_key(target, tar_gen);
      std::map<std::pair<Operation*,GenerationID>,unsigned>::const_iterator
        finder = op_map.find(target_key);
      // We only need to record it if it falls within our trace
      if (finder != op_map.end())
      {
        // Two cases here, 
        if (!source->is_internal_op())
        {
          // Normal case
          insert_dependence(DependenceRecord(finder->second, target_idx, 
                                source_idx, validates, dtype, dep_mask));
        }
        else
        {
          // Otherwise this is a internal op so record it special
          // Don't record dependences on our creator
          if (target_key != operations.back())
          { 
            std::pair<InternalOp*,GenerationID> 
              src_key(static_cast<InternalOp*>(source), src_gen);
#ifdef DEBUG_LEGION
            assert(internal_dependences.find(src_key) != 
                   internal_dependences.end());
#endif
            insert_dependence(src_key, 
                DependenceRecord(finder->second, target_idx, source_idx,
                                 validates, dtype, dep_mask));
          }
        }
      }
      else if (target->is_internal_op())
      {
        // First check to see if the internal op is one of ours
        std::pair<InternalOp*,GenerationID> 
          local_key(static_cast<InternalOp*>(target), tar_gen);
        std::map<std::pair<InternalOp*,GenerationID>,
                 LegionVector<DependenceRecord>::aligned>::const_iterator
          internal_finder = internal_dependences.find(local_key);
        if (internal_finder != internal_dependences.end())
        {
          // It is one of ours, so two cases
          if (!source->is_internal_op())
          {
            // Iterate over the internal operation dependences and 
            // translate them to our dependences
            for (LegionVector<DependenceRecord>::aligned::const_iterator
                  it = internal_finder->second.begin(); 
                  it != internal_finder->second.end(); it++)
            {
              FieldMask overlap = it->dependent_mask & dep_mask;
              if (!overlap)
                continue;
              insert_dependence(DependenceRecord(it->operation_idx, 
                  it->prev_idx, source_idx, it->validates, it->dtype, overlap));
            }
          }
          else
          {
            // Iterate over the internal operation dependences
            // and translate them to our dependences
            std::pair<InternalOp*,GenerationID> 
              src_key(static_cast<InternalOp*>(source), src_gen);
#ifdef DEBUG_LEGION
            assert(internal_dependences.find(src_key) != 
                   internal_dependences.end());
#endif
            for (LegionVector<DependenceRecord>::aligned::const_iterator
                  it = internal_finder->second.begin(); 
                  it != internal_finder->second.end(); it++)
            {
              FieldMask overlap = it->dependent_mask & dep_mask;
              if (!overlap)
                continue;
              insert_dependence(src_key, 
                  DependenceRecord(it->operation_idx, it->prev_idx,
                    source_idx, it->validates, it->dtype, overlap));
            }
          }
        }
      }
    }

    //--------------------------------------------------------------------------
    void DynamicTrace::record_aliased_children(unsigned req_index,
                                          unsigned depth, const FieldMask &mask)
    //--------------------------------------------------------------------------
    {
      unsigned index = operations.size() - 1;
      aliased_children[index].push_back(AliasChildren(req_index, depth, mask));
    } 

    //--------------------------------------------------------------------------
    void DynamicTrace::insert_dependence(const DependenceRecord &record)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!dependences.empty());
#endif
      LegionVector<DependenceRecord>::aligned &deps = dependences.back();
      // Try to merge it with an existing dependence
      for (unsigned idx = 0; idx < deps.size(); idx++)
        if (deps[idx].merge(record))
          return;
      // If we make it here, we couldn't merge it so just add it
      deps.push_back(record);
    }

    //--------------------------------------------------------------------------
    void DynamicTrace::insert_dependence(
                                 const std::pair<InternalOp*,GenerationID> &key,
                                 const DependenceRecord &record)
    //--------------------------------------------------------------------------
    {
      LegionVector<DependenceRecord>::aligned &deps = internal_dependences[key];
      // Try to merge it with an existing dependence
      for (unsigned idx = 0; idx < deps.size(); idx++)
        if (deps[idx].merge(record))
          return;
      // If we make it here, we couldn't merge it so just add it
      deps.push_back(record);
    }

    /////////////////////////////////////////////////////////////
    // TraceCaptureOp 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    TraceCaptureOp::TraceCaptureOp(Runtime *rt)
      : FenceOp(rt)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    TraceCaptureOp::TraceCaptureOp(const TraceCaptureOp &rhs)
      : FenceOp(NULL)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    TraceCaptureOp::~TraceCaptureOp(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    TraceCaptureOp& TraceCaptureOp::operator=(const TraceCaptureOp &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    void TraceCaptureOp::initialize_capture(TaskContext *ctx)
    //--------------------------------------------------------------------------
    {
      initialize(ctx, MIXED_FENCE);
#ifdef DEBUG_LEGION
      assert(trace != NULL);
      assert(trace->is_dynamic_trace());
#endif
      local_trace = trace->as_dynamic_trace();
      // Now mark our trace as NULL to avoid registering this operation
      trace = NULL;
      tracing = false;
      if (Runtime::legion_spy_enabled)
        LegionSpy::log_trace_operation(ctx->get_unique_id(), unique_op_id);
    }

    //--------------------------------------------------------------------------
    void TraceCaptureOp::activate(void)
    //--------------------------------------------------------------------------
    {
      activate_operation();
    }

    //--------------------------------------------------------------------------
    void TraceCaptureOp::deactivate(void)
    //--------------------------------------------------------------------------
    {
      deactivate_operation();
      runtime->free_capture_op(this);
    }

    //--------------------------------------------------------------------------
    const char* TraceCaptureOp::get_logging_name(void) const
    //--------------------------------------------------------------------------
    {
      return op_names[TRACE_CAPTURE_OP_KIND];
    }

    //--------------------------------------------------------------------------
    Operation::OpKind TraceCaptureOp::get_operation_kind(void) const
    //--------------------------------------------------------------------------
    {
      return TRACE_CAPTURE_OP_KIND;
    }

    //--------------------------------------------------------------------------
    void TraceCaptureOp::trigger_dependence_analysis(void)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(trace == NULL);
      assert(local_trace != NULL);
#endif
      // Indicate that we are done capturing this trace
      local_trace->end_trace_capture();
      // Register this fence with all previous users in the parent's context
      parent_ctx->perform_fence_analysis(this);
      // Now update the parent context with this fence before we can complete
      // the dependence analysis and possibly be deactivated
      parent_ctx->update_current_fence(this);
    }

    //--------------------------------------------------------------------------
    void TraceCaptureOp::trigger_mapping(void)
    //--------------------------------------------------------------------------
    {
      // Now finish capturing the physical trace
      if (local_trace->has_physical_trace() &&
          !local_trace->get_physical_trace()->is_empty())
        local_trace->get_physical_trace()->fix_trace();
      FenceOp::trigger_mapping();
    }

    /////////////////////////////////////////////////////////////
    // TraceCompleteOp 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    TraceCompleteOp::TraceCompleteOp(Runtime *rt)
      : FenceOp(rt)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    TraceCompleteOp::TraceCompleteOp(const TraceCompleteOp &rhs)
      : FenceOp(NULL)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    TraceCompleteOp::~TraceCompleteOp(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    TraceCompleteOp& TraceCompleteOp::operator=(const TraceCompleteOp &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    void TraceCompleteOp::initialize_complete(TaskContext *ctx)
    //--------------------------------------------------------------------------
    {
      initialize(ctx, MIXED_FENCE);
#ifdef DEBUG_LEGION
      assert(trace != NULL);
#endif
      local_trace = trace;
      // Now mark our trace as NULL to avoid registering this operation
      trace = NULL;
    }

    //--------------------------------------------------------------------------
    void TraceCompleteOp::activate(void)
    //--------------------------------------------------------------------------
    {
      activate_operation();
    }

    //--------------------------------------------------------------------------
    void TraceCompleteOp::deactivate(void)
    //--------------------------------------------------------------------------
    {
      deactivate_operation();
      runtime->free_trace_op(this);
    }

    //--------------------------------------------------------------------------
    const char* TraceCompleteOp::get_logging_name(void) const
    //--------------------------------------------------------------------------
    {
      return op_names[TRACE_COMPLETE_OP_KIND];
    }

    //--------------------------------------------------------------------------
    Operation::OpKind TraceCompleteOp::get_operation_kind(void) const
    //--------------------------------------------------------------------------
    {
      return TRACE_COMPLETE_OP_KIND; 
    }

    //--------------------------------------------------------------------------
    void TraceCompleteOp::trigger_dependence_analysis(void)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(trace == NULL);
      assert(local_trace != NULL);
#endif
      // Indicate that this trace is done being captured
      // This also registers that we have dependences on all operations
      // in the trace.
      local_trace->end_trace_execution(this);
      // Now update the parent context with this fence before we can complete
      // the dependence analysis and possibly be deactivated
      parent_ctx->update_current_fence(this);
      // If this is a static trace, then we remove our reference when we're done
      if (local_trace->is_static_trace())
      {
        StaticTrace *static_trace = static_cast<StaticTrace*>(local_trace);
        if (static_trace->remove_reference())
          delete static_trace;
      }
    }

    //--------------------------------------------------------------------------
    void TraceCompleteOp::trigger_mapping(void)
    //--------------------------------------------------------------------------
    {
      // Now finish capturing the physical trace
      if (local_trace->has_physical_trace() &&
          !local_trace->get_physical_trace()->is_empty() &&
          !local_trace->get_physical_trace()->is_fixed())
        local_trace->get_physical_trace()->fix_trace();
      FenceOp::trigger_mapping();
    }

    /////////////////////////////////////////////////////////////
    // PhysicalTrace
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    PhysicalTrace::PhysicalTrace()
      : fixed(false), trace_lock(Reservation::create_reservation())
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    PhysicalTrace::PhysicalTrace(const PhysicalTrace &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    PhysicalTrace::~PhysicalTrace()
    //--------------------------------------------------------------------------
    {
      trace_lock.destroy_reservation();
      trace_lock = Reservation::NO_RESERVATION;
      // Relesae references to instances
      for (CachedMappings::iterator it = cached_mappings.begin();
           it != cached_mappings.end(); ++it)
      {
        for (std::deque<InstanceSet>::iterator pit =
              it->second.physical_instances.begin(); pit !=
              it->second.physical_instances.end(); pit++)
        {
          pit->remove_valid_references(PHYSICAL_TRACE_REF);
          pit->clear();
        }
      }
      for (std::vector<PhysicalTemplate*>::iterator it = templates.begin();
           it != templates.end(); ++it)
        delete (*it);
      templates.clear();
    }

    //--------------------------------------------------------------------------
    PhysicalTrace& PhysicalTrace::operator=(const PhysicalTrace &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    void PhysicalTrace::record_target_views(PhysicalTraceInfo &trace_info,
                                            unsigned idx,
                                 const std::vector<InstanceView*> &target_views)
    //--------------------------------------------------------------------------
    {
      AutoLock t_lock(trace_lock);

      CachedViews &cached_views = cached_mappings[
        std::make_pair(trace_info.trace_local_id, trace_info.color)]
          .target_views;
      cached_views.resize(idx + 1);
      LegionVector<InstanceView*>::aligned &cache = cached_views[idx];
      cache.resize(target_views.size());
      for (unsigned i = 0; i < target_views.size(); ++i)
        cache[i] = target_views[i];
    }

    //--------------------------------------------------------------------------
    void PhysicalTrace::get_target_views(PhysicalTraceInfo &trace_info,
                                         unsigned idx,
                                 std::vector<InstanceView*> &target_views) const
    //--------------------------------------------------------------------------
    {
      AutoLock t_lock(trace_lock, 1, false/*exclusive*/);

      CachedMappings::const_iterator finder = cached_mappings.find(
          std::make_pair(trace_info.trace_local_id, trace_info.color));
#ifdef DEBUG_LEGION
      assert(finder != cached_mappings.end());
#endif
      const CachedViews &cached_views = finder->second.target_views;
      const LegionVector<InstanceView*>::aligned &cache = cached_views[idx];
      for (unsigned i = 0; i < target_views.size(); ++i)
        target_views[i] = cache[i];
    }

    //--------------------------------------------------------------------------
    void PhysicalTrace::fix_trace()
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!fixed);
#endif
      fixed = true;

      for (unsigned idx = 0; idx < templates.size(); ++idx)
      {
        printf("[Template %u]\n", idx);
        templates[idx]->execute();
        templates[idx]->clear();
      }
    }

    //--------------------------------------------------------------------------
    void PhysicalTrace::record_mapper_output(PhysicalTraceInfo &trace_info,
                                            const Mapper::MapTaskOutput &output,
                              const std::deque<InstanceSet> &physical_instances)
    //--------------------------------------------------------------------------
    {
      AutoLock t_lock(trace_lock);

      CachedMapping &mapping = cached_mappings[
        std::make_pair(trace_info.trace_local_id, trace_info.color)];
      mapping.target_procs = output.target_procs;
      mapping.chosen_variant = output.chosen_variant;
      mapping.task_priority = output.task_priority;
      mapping.postmap_task = output.postmap_task;
      mapping.physical_instances = physical_instances;
      // Hold the reference to each instance to prevent it from being collected
      for (std::deque<InstanceSet>::iterator it =
            mapping.physical_instances.begin(); it !=
            mapping.physical_instances.end(); it++)
        it->add_valid_references(PHYSICAL_TRACE_REF);
    }

    //--------------------------------------------------------------------------
    void PhysicalTrace::get_mapper_output(PhysicalTraceInfo &trace_info,
                                          VariantID &chosen_variant,
                                          TaskPriority &task_priority,
                                          bool &postmap_task,
                                          std::vector<Processor> &target_procs,
                              std::deque<InstanceSet> &physical_instances) const
    //--------------------------------------------------------------------------
    {
      AutoLock t_lock(trace_lock, 1, false/*exclusive*/);

      CachedMappings::const_iterator finder = cached_mappings.find(
          std::make_pair(trace_info.trace_local_id, trace_info.color));
#ifdef DEBUG_LEGION
      assert(finder != cached_mappings.end());
#endif
      chosen_variant = finder->second.chosen_variant;
      task_priority = finder->second.task_priority;
      postmap_task = finder->second.postmap_task;
      target_procs = finder->second.target_procs;
      physical_instances = finder->second.physical_instances;
    }

    //--------------------------------------------------------------------------
    void PhysicalTrace::start_recording_template(PhysicalTraceInfo &trace_info)
    //--------------------------------------------------------------------------
    {
      AutoLock t_lock(trace_lock);
      trace_info.template_id = templates.size();
      templates.push_back(new PhysicalTemplate());
    }

    //--------------------------------------------------------------------------
    inline PhysicalTemplate* PhysicalTrace::get_template(
                                                  PhysicalTraceInfo &trace_info)
    //--------------------------------------------------------------------------
    {
      PhysicalTemplate* tpl = NULL;
      {
        AutoLock t_lock(trace_lock, 1, false/*exclusive*/);
#ifdef DEBUG_LEGION
        assert(0 <= trace_info.template_id);
        assert(trace_info.template_id < templates.size());
#endif
        tpl = templates[trace_info.template_id];
      }
#ifdef DEBUG_LEGION
      assert(tpl != NULL);
#endif
      return tpl;
    }

    //--------------------------------------------------------------------------
    void PhysicalTrace::record_get_term_event(PhysicalTraceInfo &trace_info,
                                              ApEvent lhs,
                                              SingleTask* task)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(task->is_memoizing());
#endif
      PhysicalTemplate *tpl = get_template(trace_info);
      AutoLock tpl_lock(tpl->template_lock);

      unsigned lhs_ = tpl->events.size();
      tpl->events.push_back(lhs);
#ifdef DEBUG_LEGION
      assert(tpl->event_map.find(lhs) == tpl->event_map.end());
#endif
      tpl->event_map[lhs] = lhs_;

      unsigned rhs_ = task->get_trace_local_id();
      UniqueID rhs = task->get_unique_op_id();
      if (rhs_ >= tpl->operations.size()) tpl->operations.resize(rhs_ + 1);
      tpl->operations[rhs_] = dynamic_cast<Operation*>(task);
#ifdef DEBUG_LEGION
      assert(tpl->op_map.find(rhs) == tpl->op_map.end());
#endif
      tpl->op_map[rhs] = rhs_;
      tpl->instructions.push_back(new GetTermEvent(*tpl, lhs_, rhs_));
    }

    //--------------------------------------------------------------------------
    void PhysicalTrace::record_merge_events(PhysicalTraceInfo &trace_info,
                                            ApEvent &lhs,
                                            const std::set<ApEvent>& rhs)
    //--------------------------------------------------------------------------
    {
      if (!lhs.exists() || (rhs.find(lhs) != rhs.end()))
      {
        Realm::UserEvent rename(Realm::UserEvent::create_user_event());
        if (rhs.find(lhs) != rhs.end())
          rename.trigger(lhs);
        else
          rename.trigger();
        lhs = ApEvent(rename);
      }

      PhysicalTemplate *tpl = get_template(trace_info);
      AutoLock tpl_lock(tpl->template_lock);

      unsigned lhs_ = tpl->events.size();
      tpl->events.push_back(lhs);
#ifdef DEBUG_LEGION
      assert(tpl->event_map.find(lhs) == tpl->event_map.end());
#endif
      tpl->event_map[lhs] = lhs_;

      std::set<unsigned> rhs_;
      for (std::set<ApEvent>::const_iterator it = rhs.begin(); it != rhs.end();
           it++)
      {
        std::map<ApEvent, unsigned>::iterator finder = tpl->event_map.find(*it);
        if (finder != tpl->event_map.end())
          rhs_.insert(finder->second);
      }
      if (rhs_.size() > 0)
        tpl->instructions.push_back(new MergeEvent(*tpl, lhs_, rhs_));
      else
        tpl->instructions.push_back(new CreateNoEvent(*tpl, lhs_));
    }

    //--------------------------------------------------------------------------
    void PhysicalTrace::record_issue_copy(PhysicalTraceInfo &trace_info,
                                          ApEvent lhs,
                                          RegionTreeNode* node,
                                          Operation* op,
                         const std::vector<Domain::CopySrcDstField>& src_fields,
                         const std::vector<Domain::CopySrcDstField>& dst_fields,
                                          ApEvent precondition,
                                          PredEvent predicate_guard,
                                          RegionTreeNode *intersect,
                                          ReductionOpID redop,
                                          bool reduction_fold)
    //--------------------------------------------------------------------------
    {
      PhysicalTemplate *tpl = get_template(trace_info);
      AutoLock tpl_lock(tpl->template_lock);

      unsigned lhs_ = tpl->events.size();
      tpl->events.push_back(lhs);
#ifdef DEBUG_LEGION
      assert(tpl->event_map.find(lhs) == tpl->event_map.end());
#endif
      tpl->event_map[lhs] = lhs_;

      std::map<UniqueID, unsigned>::iterator op_finder =
        tpl->op_map.find(op->get_unique_op_id());
#ifdef DEBUG_LEGION
      assert(op_finder != tpl->op_map.end());
#endif
      unsigned op_idx = op_finder->second;

      std::map<ApEvent, unsigned>::iterator pre_finder =
        tpl->event_map.find(precondition);
#ifdef DEBUG_LEGION
      assert(pre_finder != tpl->event_map.end());
#endif
      unsigned precondition_idx = pre_finder->second;

      tpl->instructions.push_back(new IssueCopy(
            *tpl, lhs_, node, op_idx, src_fields, dst_fields, precondition_idx,
            predicate_guard, intersect, redop, reduction_fold));
    }

    /////////////////////////////////////////////////////////////
    // PhysicalTemplate
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    PhysicalTemplate::PhysicalTemplate()
      : template_lock(Reservation::create_reservation())
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    PhysicalTemplate::~PhysicalTemplate()
    //--------------------------------------------------------------------------
    {
      {
        AutoLock tpl_lock(template_lock);
        for (std::vector<Instruction*>::iterator it = instructions.begin();
             it != instructions.end(); ++it)
          delete *it;
      }
      template_lock.destroy_reservation();
      template_lock = Reservation::NO_RESERVATION;
    }

    //--------------------------------------------------------------------------
    PhysicalTemplate::PhysicalTemplate(const PhysicalTemplate &rhs)
      : template_lock(Reservation::NO_RESERVATION)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    void PhysicalTemplate::initialize()
    //--------------------------------------------------------------------------
    {
      operations.clear();
      events.clear();
    }

    //--------------------------------------------------------------------------
    void PhysicalTemplate::execute()
    //--------------------------------------------------------------------------
    {
      AutoLock tpl_lock(template_lock, 1, false/*exclusive*/);

      for (std::vector<Instruction*>::iterator it = instructions.begin();
           it != instructions.end(); ++it)
      {
        (*it)->execute();
      }
    }

    //--------------------------------------------------------------------------
    void PhysicalTemplate::clear()
    //--------------------------------------------------------------------------
    {
      event_map.clear();
      op_map.clear();
      initialize();
    }

    /////////////////////////////////////////////////////////////
    // Instruction
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    Instruction::Instruction(PhysicalTemplate& tpl)
      : operations(tpl.operations), events(tpl.events)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    void Instruction::execute()
    //--------------------------------------------------------------------------
    {
      std::cout << to_string();
    }

    /////////////////////////////////////////////////////////////
    // GetTermEvent
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    GetTermEvent::GetTermEvent(PhysicalTemplate& tpl, unsigned l, unsigned r)
      : Instruction(tpl), lhs(l), rhs(r)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(lhs < events.size());
      assert(rhs < operations.size());
#endif
    }

    //--------------------------------------------------------------------------
    std::string GetTermEvent::to_string()
    //--------------------------------------------------------------------------
    {
      std::stringstream ss;
      ss << "events[" << lhs << "] = tasks[" << rhs
         << "].get_task_termination()" << std::endl;
      return ss.str();
    }

    /////////////////////////////////////////////////////////////
    // MergeEvent
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    MergeEvent::MergeEvent(PhysicalTemplate& tpl, unsigned l,
                           const std::set<unsigned>& r)
      : Instruction(tpl), lhs(l), rhs(r)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(lhs < events.size());
      assert(rhs.size() > 0);
      for (std::set<unsigned>::iterator it = rhs.begin(); it != rhs.end();
           ++it)
        assert(*it < events.size());
#endif
    }

    //--------------------------------------------------------------------------
    std::string MergeEvent::to_string()
    //--------------------------------------------------------------------------
    {
      std::stringstream ss;
      ss << "events[" << lhs << "] = Runtime::merge_events(";
      unsigned count = 0;
      for (std::set<unsigned>::iterator it = rhs.begin(); it != rhs.end();
           ++it)
      {
        if (count++ != 0) ss << ",";
        ss << "events[" << *it << "]";
      }
      ss << ")" << std::endl;
      return ss.str();
    }

    /////////////////////////////////////////////////////////////
    // CreateNoEvent
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    CreateNoEvent::CreateNoEvent(PhysicalTemplate& tpl, unsigned l)
      : Instruction(tpl), lhs(l)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(lhs < events.size());
#endif
    }

    //--------------------------------------------------------------------------
    std::string CreateNoEvent::to_string()
    //--------------------------------------------------------------------------
    {
      std::stringstream ss;
      ss << "events[" << lhs << "] = ApEvent(Realm::Event::NO_EVENT)"
         << std::endl;
      return ss.str();
    }
    /////////////////////////////////////////////////////////////
    // IssueCopy
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    IssueCopy::IssueCopy(PhysicalTemplate& tpl,
                         unsigned l, RegionTreeNode* n, unsigned oi,
                         const std::vector<Domain::CopySrcDstField>& s,
                         const std::vector<Domain::CopySrcDstField>& d,
                         unsigned pi, PredEvent pg,
                         RegionTreeNode *i, ReductionOpID ro, bool rf)
      : Instruction(tpl), lhs(l), node(n), op_idx(oi), src_fields(s),
        dst_fields(d), precondition_idx(pi), predicate_guard(pg), intersect(i),
        redop(ro), reduction_fold(rf)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(lhs < events.size());
      assert(node->is_region());
      assert(op_idx < operations.size());
      assert(src_fields.size() > 0);
      assert(dst_fields.size() > 0);
      assert(precondition_idx < events.size());
#endif
    }

    //--------------------------------------------------------------------------
    std::string IssueCopy::to_string()
    //--------------------------------------------------------------------------
    {
      std::stringstream ss;
      ss << "events[" << lhs << "] = ";
      LogicalRegion handle = node->as_region_node()->handle;
      ss << "(" << handle.get_index_space().get_id()
         << "," << handle.get_field_space().get_id()
         << "," << handle.get_tree_id()
         << ")->issue_copy(tasks[" << op_idx << "], ";
      ss << "{";
      for (unsigned idx = 0; idx < src_fields.size(); ++idx)
      {
        ss << "(" << std::hex << src_fields[idx].inst.id
           << "," << std::dec << src_fields[idx].offset
           << "," << src_fields[idx].size
           << "," << src_fields[idx].field_id
           << "," << src_fields[idx].serdez_id << ")";
        if (idx != src_fields.size() - 1) ss << ",";
      }
      ss << "}, {";
      for (unsigned idx = 0; idx < dst_fields.size(); ++idx)
      {
        ss << "(" << std::hex << dst_fields[idx].inst.id
           << "," << std::dec << dst_fields[idx].offset
           << "," << dst_fields[idx].size
           << "," << dst_fields[idx].field_id
           << "," << dst_fields[idx].serdez_id << ")";
        if (idx != dst_fields.size() - 1) ss << ",";
      }
      ss << "}, events[" << precondition_idx << "]";
      if (intersect != NULL)
      {
        if (intersect->is_region())
        {
          LogicalRegion handle = node->as_region_node()->handle;
          ss << ", lr(" << handle.get_index_space().get_id()
             << "," << handle.get_field_space().get_id()
             << "," << handle.get_tree_id() << ")";
        }
        else
        {
          LogicalPartition handle = node->as_partition_node()->handle;
          ss << ", lp(" << handle.get_index_partition().get_id()
             << "," << handle.get_field_space().get_id()
             << "," << handle.get_tree_id() << ")";
        }
      }

      if (redop != 0) ss << ", " << redop;
      ss << ")\n";

      return ss.str();
    }

    /////////////////////////////////////////////////////////////
    // PhysicalTraceInfo
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    PhysicalTraceInfo::PhysicalTraceInfo()
    //--------------------------------------------------------------------------
      : memoizing(false), tracing(false), is_point_task(false),
        trace_local_id(0), color(), trace(NULL)
    {
    }

  }; // namespace Internal 
}; // namespace Legion

