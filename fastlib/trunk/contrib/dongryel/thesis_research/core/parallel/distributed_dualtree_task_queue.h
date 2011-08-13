/** @file distributed_dualtree_task_queue.h
 *
 *  @author Dongryeol Lee (dongryel@cc.gatech.edu)
 */

#ifndef CORE_PARALLEL_DISTRIBUTED_DUALTREE_TASK_QUEUE_H
#define CORE_PARALLEL_DISTRIBUTED_DUALTREE_TASK_QUEUE_H

#include <deque>
#include <vector>
#include "core/math/range.h"
#include "core/parallel/disjoint_int_intervals.h"
#include "core/parallel/table_exchange.h"

namespace core {
namespace parallel {

template<typename DistributedTableType, typename TaskPriorityQueueType>
class DistributedDualtreeTaskQueue {
  public:

    /** @brief The table type used in the exchange process.
     */
    typedef typename DistributedTableType::TableType TableType;

    typedef typename TableType::TreeType TreeType;

    /** @brief The subtable type used in the exchange process.
     */
    typedef core::table::SubTable<TableType> SubTableType;

    typedef core::parallel::RouteRequest<SubTableType> SubTableRouteRequestType;

    typedef core::parallel::TableExchange <
    DistributedTableType, TaskPriorityQueueType > TableExchangeType;

  private:

    std::vector< TreeType *> query_subtrees_;

    std::vector< core::parallel::DisjointIntIntervals * > assigned_work_;

    std::vector< unsigned long int > remaining_work_for_query_subtrees_;

    std::deque<bool> query_subtree_locks_;

    std::vector<TaskPriorityQueueType *> tasks_;

    bool split_subtree_after_unlocking_;

    TableExchangeType table_exchange_;

    int num_remaining_tasks_;

    unsigned long int remaining_global_computation_;

    unsigned long int remaining_local_computation_;

  private:

    template<typename MetricType>
    void split_subtree_(
      boost::mpi::communicator &world,
      const MetricType &metric_in, int subtree_index) {

      // After splitting, the current index will have the left child
      // and the right child will be appended to the end of the list
      // of trees, plus duplicating the reference tasks along the way.
      TreeType *prev_qnode = query_subtrees_[subtree_index];
      TreeType *left = query_subtrees_[subtree_index]->left();
      TreeType *right = query_subtrees_[subtree_index]->right();

      // Overwrite with the left child.
      query_subtrees_[subtree_index] = left;

      // Grow the list of local query subtrees.
      query_subtrees_.push_back(right);
      query_subtree_locks_.push_back(false);

      // Adjust the list of tasks.
      std::vector<TaskType> prev_tasks;
      while(tasks_[subtree_index]->size() > 0) {
        std::pair<TaskType, int> task_pair;
        this->DequeueTask(subtree_index, &task_pair, false);
        prev_tasks.push_back(task_pair.first);
      }
      tasks_.push_back(new TaskPriorityQueueType());
      assigned_work_.push_back(
        new core::parallel::DisjointIntIntervals(
          world, *(assigned_work_[subtree_index])));
      remaining_work_for_query_subtrees_.push_back(
        remaining_work_for_query_subtrees_[ subtree_index]);
      for(unsigned int i = 0; i < prev_tasks.size(); i++) {

        // If the previous query node is equal to the reference node,
        // then split the reference as well.
        if((! prev_tasks[i].reference_start_node()->is_leaf()) &&
            prev_qnode == prev_tasks[i].reference_start_node()) {
          TreeType *reference_left_child =
            prev_tasks[i].reference_start_node()->left();
          TreeType *reference_right_child =
            prev_tasks[i].reference_start_node()->right();
          boost::tuple <
          TableType *, TreeType *, int > reference_table_left_node_pair(
            prev_tasks[i].reference_table(),
            reference_left_child, prev_tasks[i].cache_id());
          boost::tuple <
          TableType *, TreeType *, int > reference_table_right_node_pair(
            prev_tasks[i].reference_table(),
            reference_right_child, prev_tasks[i].cache_id());
          this->PushTask(
            metric_in, subtree_index, reference_table_left_node_pair);
          this->PushTask(
            metric_in, subtree_index, reference_table_right_node_pair);
          this->PushTask(
            metric_in, query_subtrees_.size() - 1,
            reference_table_left_node_pair);
          this->PushTask(
            metric_in, query_subtrees_.size() - 1,
            reference_table_right_node_pair);

          // Lock three more times since the reference side is also
          // split.
          table_exchange_.LockCache(prev_tasks[i].cache_id(), 3);
        }
        else {

          boost::tuple<TableType *, TreeType *, int> reference_table_node_pair(
            prev_tasks[i].reference_table(),
            prev_tasks[i].reference_start_node(), prev_tasks[i].cache_id());
          this->PushTask(metric_in, subtree_index, reference_table_node_pair);
          this->PushTask(
            metric_in, query_subtrees_.size() - 1,
            reference_table_node_pair);

          // Lock only one time since only the query side is split.
          table_exchange_.LockCache(prev_tasks[i].cache_id(), 1);
        }
      }
    }

  public:

    typedef typename TaskPriorityQueueType::value_type TaskType;

  public:

    /** @brief The destructor.
     */
    ~DistributedDualtreeTaskQueue() {
      for(unsigned int i = 0; i < assigned_work_.size(); i++) {
        delete assigned_work_[i];
        delete tasks_[i];
      }
      assigned_work_.resize(0);
      tasks_.resize(0);
    }

    unsigned long int &remaining_global_computation() {
      return remaining_global_computation_;
    }

    void ReleaseCache(int cache_id, int num_times) {
      table_exchange_.ReleaseCache(cache_id, num_times);
    }

    template<typename MetricType>
    void SendReceive(
      int thread_id,
      const MetricType &metric_in,
      boost::mpi::communicator &world,
      DistributedTableType *reference_table_in,
      std::vector <
      SubTableRouteRequestType > &hashed_essential_reference_subtrees_to_send) {
      table_exchange_.SendReceive(
        thread_id, metric_in, world, reference_table_in,
        hashed_essential_reference_subtrees_to_send);
    }

    template<typename MetricType>
    void GenerateTasks(
      const MetricType &metric_in,
      const std::vector <
      boost::tuple<int, int, int, int> > &received_subtable_ids) {
      for(unsigned int i = 0; i < received_subtable_ids.size(); i++) {

        // Find the reference process ID and grab its subtable.
        int reference_begin = received_subtable_ids[i].get<1>();
        int reference_count = received_subtable_ids[i].get<2>();
        int cache_id = received_subtable_ids[i].get<3>();
        SubTableType *frontier_reference_subtable =
          table_exchange_.FindSubTable(cache_id);

        // Find the table and the starting reference node.
        TableType *frontier_reference_table =
          (frontier_reference_subtable != NULL) ?
          frontier_reference_subtable->table() :
          table_exchange_.local_table();
        TreeType *reference_starting_node =
          (frontier_reference_subtable != NULL) ?
          frontier_reference_subtable->table()->get_tree() :
          table_exchange_.FindByBeginCount(
            reference_begin, reference_count);
        boost::tuple<TableType *, TreeType *, int> reference_table_node_pair(
          frontier_reference_table, reference_starting_node, cache_id);

        // For each query subtree, create a new task if it has not
        // already taken care of the incoming reference table.
        for(int j = 0; j < this->size(); j++) {
          if(assigned_work_[j]->Insert(
                boost::tuple<int, int, int>(
                  frontier_reference_table->rank(),
                  reference_begin,
                  reference_begin + reference_count))) {
            this->PushTask(metric_in, j, reference_table_node_pair);
            table_exchange_.LockCache(cache_id, 1);
          }
        }

      } //end of looping over each reference subtree.
    }

    bool can_terminate() const {
      return remaining_global_computation_ == 0 &&
             table_exchange_.can_terminate();
    }

    void push_completed_computation(
      const boost::tuple<int, int, int> &query_node_id,
      boost::mpi::communicator &comm,
      unsigned long int reference_count_in,
      unsigned long int quantity_in) {

      // Subtract from the self and queue up a route message.
      remaining_global_computation_ -= quantity_in;
      remaining_local_computation_ -= quantity_in;
      table_exchange_.push_completed_computation(comm, quantity_in);

      // Update the remaining work for the query tree. Maybe the
      // searching can be sped up later.
      int found_index = -1;
      for(unsigned int i = 0;
          found_index < 0 && i < query_subtrees_.size(); i++) {
        if(query_node_id.get<1>() == query_subtrees_[i]->begin() &&
            query_node_id.get<2>() == query_subtrees_[i]->count()) {
          found_index = i;
        }
      }
      remaining_work_for_query_subtrees_[found_index] -= reference_count_in;
    }

    void push_completed_computation(
      boost::mpi::communicator &comm,
      unsigned long int reference_count_in,
      unsigned long int quantity_in) {

      // Subtract from the self and queue up a route message.
      remaining_global_computation_ -= quantity_in;
      remaining_local_computation_ -= quantity_in;
      table_exchange_.push_completed_computation(comm, quantity_in);

      // Update the remaining work for all of the existing query
      // trees.
      for(unsigned int i = 0; i < query_subtrees_.size(); i++) {
        remaining_work_for_query_subtrees_[i] -= reference_count_in;
      }
    }

    int num_remaining_tasks() const {
      return num_remaining_tasks_;
    }

    bool is_empty() const {
      return num_remaining_tasks_ == 0;
    }

    void set_split_subtree_flag() {
      split_subtree_after_unlocking_ = true;
    }

    /** @brief The constructor.
     */
    DistributedDualtreeTaskQueue() {
      num_remaining_tasks_ = 0;
      remaining_global_computation_ = 0;
      remaining_local_computation_ = 0;
      split_subtree_after_unlocking_ = false;
    }

    int size() const {
      return query_subtrees_.size();
    }

    template<typename MetricType>
    void RedistributeAmongCores(
      boost::mpi::communicator &world,
      DistributedTableType *reference_table_in,
      const MetricType &metric_in) {

      // If the splitting was requested,
      if(split_subtree_after_unlocking_) {

        // Try to find a subtree to split.
        int split_index_query_size = 0;
        int split_index = -1;
        for(unsigned int i = 0; i < query_subtrees_.size(); i++) {
          if((! query_subtree_locks_[i]) &&
              (! query_subtrees_[i]->is_leaf()) &&
              tasks_[i]->size() > 0 &&
              split_index_query_size < query_subtrees_[i]->count())  {
            split_index_query_size = query_subtrees_[i]->count();
            split_index = i;
          }
        }
        if(split_index >= 0) {
          split_subtree_(world, metric_in, split_index);
        }
        split_subtree_after_unlocking_ = false;
      }

      // Cleanup completed query subtrees.
      for(int i = 0; i < static_cast<int>(query_subtrees_.size()); i++) {
        if(remaining_work_for_query_subtrees_[i] == 0) {
          delete assigned_work_[i];
          delete tasks_[i];
          query_subtrees_[i] = query_subtrees_.back();
          assigned_work_[i] = assigned_work_.back();
          remaining_work_for_query_subtrees_[i] =
            remaining_work_for_query_subtrees_.back();
          query_subtree_locks_[i] = query_subtree_locks_.back();
          tasks_[i] = tasks_.back();

          query_subtrees_.pop_back();
          assigned_work_.pop_back();
          remaining_work_for_query_subtrees_.pop_back();
          query_subtree_locks_.pop_back();
          tasks_.pop_back();
          i--;
        }
      }
    }

    template<typename MetricType>
    void UnlockQuerySubtree(const MetricType &metric_in, int subtree_index) {

      // Unlock the query subtree.
      query_subtree_locks_[ subtree_index ] = false;
    }

    void Init(
      boost::mpi::communicator &world,
      DistributedTableType *query_table_in,
      DistributedTableType *reference_table_in,
      int num_threads_in) {

      // For each process, break up the local query tree into a list of
      // subtree query lists.
      query_table_in->local_table()->get_frontier_nodes_bounded_by_number(
        num_threads_in, &query_subtrees_);

      // Initialize the other member variables.
      query_subtree_locks_.resize(query_subtrees_.size());
      tasks_.resize(query_subtrees_.size());
      split_subtree_after_unlocking_ = false;
      for(unsigned int i = 0; i < query_subtrees_.size(); i++) {
        query_subtree_locks_[i] = false;
        tasks_[i] = new TaskPriorityQueueType();
      }

      // Initialize the table exchange.
      table_exchange_.Init(world, query_table_in, reference_table_in, this);

      // Initialize the amount of remaining computation.
      unsigned long int total_num_query_points = 0;
      unsigned long int total_num_reference_points = 0;
      for(int i = 0; i < world.size(); i++) {
        total_num_query_points += query_table_in->local_n_entries(i);
        total_num_reference_points += reference_table_in->local_n_entries(i);
      }

      // Initialize the remaining computation.
      remaining_global_computation_ =
        static_cast<unsigned long int>(total_num_query_points) *
        static_cast<unsigned long int>(total_num_reference_points);
      remaining_local_computation_ =
        static_cast<unsigned long int>(
          query_table_in->local_table()->n_entries()) *
        static_cast<unsigned long int>(total_num_reference_points);

      // Initialize the completed computation grid for each query tree
      // on this process.
      assigned_work_.resize(query_subtrees_.size()) ;
      remaining_work_for_query_subtrees_.resize(query_subtrees_.size());
      for(unsigned int i = 0; i < query_subtrees_.size(); i++) {
        assigned_work_[i] = new core::parallel::DisjointIntIntervals();
        assigned_work_[i]->Init(world);
        remaining_work_for_query_subtrees_[i] = total_num_reference_points;
      }
    }

    template<typename MetricType>
    void PushTask(
      const MetricType &metric_in,
      int push_index,
      boost::tuple<TableType *, TreeType *, int> &reference_table_node_pair) {

      // Compute the priority and push in.
      core::math::Range squared_distance_range(
        query_subtrees_[push_index]->bound().RangeDistanceSq(
          metric_in, reference_table_node_pair.get<1>()->bound()));
      TaskType new_task(
        query_subtrees_[push_index],
        reference_table_node_pair.get<0>(),
        reference_table_node_pair.get<1>(),
        reference_table_node_pair.get<2>(),
        - squared_distance_range.mid());
      tasks_[ push_index]->push(new_task);

      // Increment the number of tasks.
      num_remaining_tasks_++;
    }

    /** @brief Dequeues a task, optionally locking a query subtree
     *         associated with it.
     */
    void DequeueTask(
      int probe_index, std::pair<TaskType, int> *task_out,
      bool lock_query_subtree_in) {

      // Try to dequeue a task from the given query subtree if it is
      // not locked yet. Otherwise, request it to be split in the next
      // iteration.
      if(probe_index < static_cast<int>(tasks_.size()) &&
          tasks_[probe_index]->size() > 0) {
        if(! query_subtree_locks_[ probe_index ]) {

          // Copy the task and the query subtree number.
          task_out->first = tasks_[ probe_index ]->top();
          task_out->second = probe_index;

          // Pop the task from the priority queue after copying and
          // put a lock on the query subtree.
          tasks_[ probe_index ]->pop();
          query_subtree_locks_[ probe_index ] = lock_query_subtree_in;

          // Decrement the number of tasks.
          num_remaining_tasks_--;
        }
      }
    }
};
}
}

#endif
