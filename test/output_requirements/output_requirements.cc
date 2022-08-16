/* Copyright 2022 Stanford University
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

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>

#include "legion.h"

#include "mappers/default_mapper.h"

using namespace std;
using namespace Legion;
using namespace Legion::Mapping;

#define SIZE 10

enum FIDs
{
  FID_X = 100,
  FID_Y = 101,
  FID_Z = 102,
  FID_W = 103,
};

enum TaskIDs
{
  TID_MAIN = 100,
  TID_PRODUCER_GLOBAL = 101,
  TID_PRODUCER_LOCAL = 102,
  TID_CONSUMER_GLOBAL = 103,
  TID_CONSUMER_LOCAL = 104,
  TID_CONDITION = 105,
};

enum MappingTags
{
  TAG_REUSE = 100,
  TAG_CREATE_NEW = 101,
  TAG_LOCAL_PROCESSOR = 102,
};

class OutReqTestMapper : public DefaultMapper
{
 public:
  OutReqTestMapper(MapperRuntime *rt, Machine machine, Processor local,
                      const char *mapper_name);
 public:
  virtual LogicalRegion default_policy_select_instance_region(
      MapperContext, Memory, const RegionRequirement &req,
      const LayoutConstraintSet&, bool, bool)
  {
    return req.region;
  }
  virtual void default_policy_select_instance_fields(
                                MapperContext ctx,
                                const RegionRequirement &req,
                                const std::set<FieldID> &needed_fields,
                                std::vector<FieldID> &fields)
  {
    fields.insert(fields.end(), needed_fields.begin(), needed_fields.end());
  }

 public:
  virtual int default_policy_select_garbage_collection_priority(
                                    MapperContext ctx,
                                    MappingKind kind, Memory memory,
                                    const PhysicalInstance &instance,
                                    bool meets_fill_constraints,
                                    bool reduction)
  {
    return LEGION_GC_FIRST_PRIORITY;
  }

 public:
  using DefaultMapper::speculate;
  virtual void select_task_options(const MapperContext    ctx,
                                   const Task&            task,
                                         TaskOptions&     output);
  virtual void speculate(const MapperContext      ctx,
                         const Task&              task,
                               SpeculativeOutput& output);
  virtual void slice_task(const MapperContext ctx,
                          const Task& task,
                          const SliceTaskInput& input,
                          SliceTaskOutput& output);
  virtual void map_task(const MapperContext ctx,
                        const Task& task,
                        const MapTaskInput& input,
                        MapTaskOutput& output);
 private:
  Memory local_sysmem;
  bool request_speculate;
  std::map<TaskIDs, Processor> producer_mappings;
};

OutReqTestMapper::OutReqTestMapper(MapperRuntime *rt,
                                         Machine machine,
                                         Processor local,
                                         const char *mapper_name)
  : DefaultMapper(rt, machine, local, mapper_name),
    request_speculate(false)
{
  Machine::MemoryQuery visible_memories(machine);
  visible_memories.has_affinity_to(local);
  visible_memories.only_kind(Memory::SYSTEM_MEM);
  local_sysmem = visible_memories.first();

  const InputArgs &command_args = Runtime::get_input_args();
  char **argv = command_args.argv;
  int argc = command_args.argc;

  for (int i = 0; i < argc; ++i)
    if (strcmp(argv[i], "-speculate") == 0)
      request_speculate = true;
}

void OutReqTestMapper::select_task_options(const MapperContext    ctx,
                                           const Task&            task,
                                                 TaskOptions&     output)
{
  DefaultMapper::select_task_options(ctx, task, output);
  if (task.task_id == TID_PRODUCER_GLOBAL || task.task_id == TID_PRODUCER_LOCAL)
    producer_mappings[static_cast<TaskIDs>(task.task_id)] = output.initial_proc;
  else if (task.tag == TAG_LOCAL_PROCESSOR)
  {
    if (task.task_id == TID_CONSUMER_GLOBAL)
      output.initial_proc = producer_mappings[TID_PRODUCER_GLOBAL];
    else
    {
      assert(task.task_id == TID_CONSUMER_LOCAL);
      output.initial_proc = producer_mappings[TID_PRODUCER_LOCAL];
    }
  }
}

void OutReqTestMapper::speculate(const MapperContext      ctx,
                                 const Task&              task,
                                       SpeculativeOutput& output)
{
  if (task.task_id == TID_PRODUCER_GLOBAL || task.task_id == TID_PRODUCER_LOCAL)
    output.speculate = request_speculate;
  else
    output.speculate = false;
}

void OutReqTestMapper::slice_task(const MapperContext ctx,
                                     const Task& task,
                                     const SliceTaskInput& input,
                                     SliceTaskOutput& output)
{
  size_t idx = 0;
  for (Domain::DomainPointIterator itr(input.domain); itr; ++itr, ++idx) {
    Domain slice(*itr, *itr);
    output.slices.push_back(
      TaskSlice(slice, local_cpus[idx % local_cpus.size()], false, false)
    );
  }
}

void OutReqTestMapper::map_task(const MapperContext ctx,
                                const Task& task,
                                const MapTaskInput& input,
                                MapTaskOutput& output)
{
  if (!(task.task_id == TID_PRODUCER_GLOBAL
        || task.task_id == TID_PRODUCER_LOCAL
        || task.task_id == TID_CONSUMER_GLOBAL
        || task.task_id == TID_CONSUMER_LOCAL))
  {
    DefaultMapper::map_task(ctx, task, input, output);
    return;
  }

  output.task_priority = 0;
  output.postmap_task  = false;
  output.target_procs.push_back(task.target_proc);
  std::vector<VariantID> variants;
  runtime->find_valid_variants(
      ctx, task.task_id, variants, Processor::LOC_PROC);
  assert(!variants.empty());
  output.chosen_variant = *variants.begin();

  if (task.task_id == TID_PRODUCER_GLOBAL)
  {
    output.output_targets[0] = local_sysmem;

    LayoutConstraintSet &constraints = output.output_constraints[0];

    std::vector<DimensionKind> ordering;
    ordering.push_back(DIM_X);
    ordering.push_back(DIM_Y);
    ordering.push_back(DIM_F);
    constraints.ordering_constraint = OrderingConstraint(ordering, false);

    constraints.alignment_constraints.push_back(
      AlignmentConstraint(FID_X, LEGION_EQ_EK, 32));

    return;
  }
  else if (task.task_id == TID_PRODUCER_LOCAL)
  {
    output.output_targets[0] = local_sysmem;

    LayoutConstraintSet &constraints = output.output_constraints[0];

    std::vector<DimensionKind> ordering;
    if (task.is_index_space)
    {
      ordering.push_back(DIM_Z);
      ordering.push_back(DIM_Y);
      ordering.push_back(DIM_X);
      ordering.push_back(DIM_F);
    }
    else
    {
      ordering.push_back(DIM_Y);
      ordering.push_back(DIM_X);
      ordering.push_back(DIM_F);
    }
    constraints.ordering_constraint = OrderingConstraint(ordering, false);

    return;
  }

  const RegionRequirement &req = task.regions[0];
  std::vector<LogicalRegion> regions(1, req.region);
  std::vector<DimensionKind> ordering;
  if (task.task_id == TID_CONSUMER_GLOBAL)
  {
    ordering.push_back(DIM_X);
    ordering.push_back(DIM_Y);
    ordering.push_back(DIM_F);
  }
  else
  {
    assert(task.task_id == TID_CONSUMER_LOCAL);
    if (task.is_index_space)
    {
      ordering.push_back(DIM_Z);
      ordering.push_back(DIM_Y);
      ordering.push_back(DIM_X);
      ordering.push_back(DIM_F);
    }
    else
    {
      ordering.push_back(DIM_Y);
      ordering.push_back(DIM_X);
      ordering.push_back(DIM_F);
    }
  }

  if (req.tag == TAG_REUSE)
  {
    for (unsigned idx = 0; idx < req.instance_fields.size(); ++idx)
    {
      std::vector<FieldID> fields(1, req.instance_fields[idx]);
      LayoutConstraintSet constraints;
      constraints.add_constraint(MemoryConstraint(local_sysmem.kind()))
        .add_constraint(OrderingConstraint(ordering, false))
        .add_constraint(FieldConstraint(fields, false, false))
        .add_constraint(
          SpecializedConstraint(LEGION_AFFINE_SPECIALIZE, 0, false, true));

      PhysicalInstance instance;
      assert(runtime->find_physical_instance(ctx,
                                             local_sysmem,
                                             constraints,
                                             regions,
                                             instance,
                                             true,
                                             true));
      output.chosen_instances[0].push_back(instance);
    }
  }
  else
  {
    assert(req.tag == TAG_CREATE_NEW);
    LayoutConstraintSet constraints;
    constraints.add_constraint(MemoryConstraint(local_sysmem.kind()))
      .add_constraint(OrderingConstraint(ordering, false))
      .add_constraint(FieldConstraint(req.instance_fields, false, false))
      .add_constraint(
        SpecializedConstraint(LEGION_AFFINE_SPECIALIZE, 0, false, true));

    PhysicalInstance instance;
    size_t footprint;
    assert(runtime->create_physical_instance(ctx,
                                             local_sysmem,
                                             constraints,
                                             regions,
                                             instance,
                                             true,
                                             0,
                                             true,
                                             &footprint));
    output.chosen_instances[0].push_back(instance);
  }
}

struct TestArgs {
  TestArgs(void)
    : index_launch(false), predicate(false), empty(false), replicate(false)
  { }
  bool index_launch;
  bool predicate;
  bool empty;
  bool replicate;
};

bool condition_task(const Task *task,
                     const std::vector<PhysicalRegion> &regions,
                     Context ctx,
                     Runtime *runtime)
{
  usleep(2000);
  return false;
}

void producer_global_task(const Task *task,
                          const std::vector<PhysicalRegion> &regions,
                          Context ctx,
                          Runtime *runtime)
{
  static constexpr int DIM = 2;

  TestArgs *args = reinterpret_cast<TestArgs*>(task->args);

  std::vector<OutputRegion> outputs;
  runtime->get_output_regions(ctx, outputs);
  OutputRegion& output = outputs.front();

  if (args->empty)
  {
    Rect<DIM, int32_t> bounds(Point<DIM, int32_t>::ONES(), Point<DIM, int32_t>::ZEROES());
    DeferredBuffer<int64_t, 2, int32_t> buf_x(bounds, Memory::Kind::SYSTEM_MEM, NULL, 32, true);
    output.return_data(bounds.hi, FID_X, buf_x);
    outputs[0].create_buffer<int32_t, 2>(bounds.hi, FID_Y, NULL, true);
    return;
  }

  Point<DIM, int32_t> extents;
  if (task->is_index_space)
    for (int32_t dim = 0; dim < DIM; ++dim)
      extents[dim] = SIZE - task->index_point[dim];
  else
    for (int32_t dim = 0; dim < DIM; ++dim)
      extents[dim] = SIZE;

  size_t volume = 1;
  for (int32_t dim = 0; dim < DIM; ++dim) volume *= extents[dim];

  Point<DIM, int32_t> hi(extents);
  hi -= Point<DIM, int32_t>::ONES();
  Rect<DIM, int32_t> bounds(Point<DIM, int32_t>::ZEROES(), hi);

  DeferredBuffer<int64_t, 2, int32_t> buf_x(
    bounds, Memory::Kind::SYSTEM_MEM, NULL, 32, true);
  DeferredBuffer<int32_t, 2, int32_t> buf_y =
    outputs[0].create_buffer<int32_t, 2>(extents, FID_Y, NULL, true);

  int64_t *ptr_x = buf_x.ptr(Point<2, int32_t>::ZEROES());
  int32_t *ptr_y = buf_y.ptr(Point<2, int32_t>::ZEROES());

  for (size_t idx = 0; idx < volume; ++idx)
  {
    ptr_x[idx] = 111 + static_cast<int64_t>(idx);
    ptr_y[idx] = 222 + static_cast<int32_t>(idx);
  }

  output.return_data(extents, FID_X, buf_x);
}

void producer_local_task(const Task *task,
                         const std::vector<PhysicalRegion> &regions,
                         Context ctx,
                         Runtime *runtime)
{
  static constexpr int DIM = 2;

  TestArgs *args = reinterpret_cast<TestArgs*>(task->args);

  std::vector<OutputRegion> outputs;
  runtime->get_output_regions(ctx, outputs);
  OutputRegion& output = outputs.front();

  if (args->empty)
  {
    Rect<DIM, int32_t> bounds(
        Point<DIM, int32_t>::ONES(), Point<DIM, int32_t>::ZEROES());
    DeferredBuffer<int64_t, DIM, int32_t> buf_z(
        bounds, Memory::Kind::SYSTEM_MEM);
    output.return_data(bounds.hi, FID_Z, buf_z);
    // TODO: Currently we can't return a buffer to FID_W, as deferred buffers
    //       of a zero-size field cannot be created. Put back this test case
    //       once we add APIs to return untyped buffers to output regions.
    // DeferredBuffer<int8_t, DIM, int32_t> buf_w(
    //   bounds, Memory::Kind::SYSTEM_MEM);
    // output.return_data(bounds.hi, FID_W, buf_w);
    return;
  }

  Point<DIM, int32_t> extents;
  if (task->is_index_space)
    for (int32_t dim = 0; dim < DIM; ++dim)
      extents[dim] = SIZE - task->index_point[0];
  else
    for (int32_t dim = 0; dim < DIM; ++dim)
      extents[dim] = SIZE;

  size_t volume = 1;
  for (int32_t dim = 0; dim < DIM; ++dim) volume *= extents[dim];

  Point<DIM, int32_t> hi(extents);
  hi -= Point<DIM>::ONES();
  Rect<DIM, int32_t> bounds(Point<DIM>::ZEROES(), hi);

  DeferredBuffer<int64_t, DIM, int32_t> buf_z(
    bounds, Memory::Kind::SYSTEM_MEM, NULL, 16, false);
  int64_t *ptr_z = buf_z.ptr(Point<2, int32_t>::ZEROES());

  for (size_t idx = 0; idx < volume; ++idx)
    ptr_z[idx] = 333 + static_cast<int64_t>(idx);

  output.return_data(extents, FID_Z, buf_z);

  // TODO: Currently we can't return a buffer to FID_W, as deferred buffers
  //       of a zero-size field cannot be created. Put back this test case
  //       once we add APIs to return untyped buffers to output regions.
  //DeferredBuffer<int8_t, DIM, int32_t> buf_w(
  //  bounds, Memory::Kind::SYSTEM_MEM, NULL, 16, false);
  //output.return_data(extents, FID_W, buf_w);
}

typedef FieldAccessor<READ_ONLY, int64_t, 2, int32_t,
                      Realm::AffineAccessor<int64_t, 2, int32_t> >
        Int64Accessor2D;

typedef FieldAccessor<READ_ONLY, int32_t, 2, int32_t,
                      Realm::AffineAccessor<int32_t, 2, int32_t> >
        Int32Accessor2D;

typedef FieldAccessor<READ_ONLY, int64_t, 3, int32_t,
                      Realm::AffineAccessor<int64_t, 3, int32_t> >
        Int64Accessor3D;

void consumer_global_task(const Task *task,
                          const std::vector<PhysicalRegion> &regions,
                          Context ctx,
                          Runtime *runtime)
{
  static constexpr int DIM = 2;

  TestArgs *args = reinterpret_cast<TestArgs*>(task->args);

  Rect<DIM, int32_t> r(regions[0]);
  std::cerr << "[Consumer " << task->index_point
            << ", global indexing] region: " << r << std::endl;

  if (args->empty || args->predicate)
  {
    assert(r.empty());
    return;
  }

  if (args->index_launch)
  {
    Rect<DIM, int32_t> r(regions[0]);
    static int32_t offsets[] = {0, SIZE, 2 * SIZE - 1, 3 * SIZE - 3};

    for (int32_t dim = 0; dim < DIM; ++dim)
    {
      assert(r.lo[dim] == offsets[task->index_point[dim]]);
      assert(r.hi[dim] == offsets[task->index_point[dim] + 1] - 1);
    }
  }
  else
  {
    Rect<DIM, int32_t> r(regions[0]);
    for (int32_t dim = 0; dim < DIM; ++dim)
    {
      assert(r.lo[dim] == 0);
      assert(r.hi[dim] == SIZE - 1);
    }
  }

  Int64Accessor2D acc_x(regions[0], FID_X);
  Int32Accessor2D acc_y(regions[0], FID_Y);

  Point<DIM, int32_t> extents = r.hi;
  extents -= r.lo;
  extents += Point<DIM, int32_t>::ONES();

  int32_t volume = r.volume();
  for (int32_t idx = 0; idx < volume; ++idx)
  {
    int32_t x0 = idx % extents[0];
    int32_t x1 = idx / extents[0];
    Point<2, int32_t> p(x0, x1);
    assert(acc_x[p + r.lo] == 111 + idx);
    assert(acc_y[p + r.lo] == 222 + idx);
  }
}

void consumer_local_task(const Task *task,
                         const std::vector<PhysicalRegion> &regions,
                         Context ctx,
                         Runtime *runtime)
{
  TestArgs *args = reinterpret_cast<TestArgs*>(task->args);

  if (args->index_launch)
  {
    static constexpr int DIM = 3;

    Rect<DIM, int32_t> r(regions[0]);
    std::cerr << "[Consumer " << task->index_point
              << ", local indexing] region: " << r << std::endl;
    if (args->empty || args->predicate)
    {
      assert(r.empty());
      return;
    }

    assert(r.lo[0] == task->index_point[0]);
    assert(r.hi[0] == task->index_point[0]);
    for (int32_t dim = 0; dim < DIM - 1; ++dim)
    {
      assert(r.lo[dim + 1] == 0);
      assert(r.hi[dim + 1] == SIZE - task->index_point[0] - 1);
    }

    Int64Accessor3D acc_z(regions[0], FID_Z);

    Point<DIM, int32_t> extents = r.hi;
    extents -= r.lo;
    extents += Point<DIM, int32_t>::ONES();

    int32_t volume = r.volume();
    for (int32_t idx = 0; idx < volume; ++idx)
    {
      int32_t x0 = idx / extents[2];
      int32_t x1 = idx % extents[2];
      Point<3, int32_t> p(task->index_point[0], x0 + r.lo[1], x1 + r.lo[2]);
      assert(acc_z[p] == 333 + idx);
    }
  }
  else
  {
    static constexpr int DIM = 2;

    Rect<DIM, int32_t> r(regions[0]);
    std::cerr << "[Consumer " << task->index_point
              << ", local indexing] region: " << r << std::endl;
    if (args->empty || args->predicate)
    {
      assert(r.empty());
      return;
    }

    for (int32_t dim = 0; dim < DIM; ++dim)
    {
      assert(r.lo[dim] == 0);
      assert(r.hi[dim] == SIZE - 1);
    }

    Int64Accessor2D acc_z(regions[0], FID_Z);

    Point<DIM, int32_t> extents = r.hi;
    extents -= r.lo;
    extents += Point<DIM>::ONES();

    int32_t volume = r.volume();
    for (int32_t idx = 0; idx < volume; ++idx)
    {
      int32_t x0 = idx / extents[1];
      int32_t x1 = idx % extents[1];
      Point<2, int32_t> p(x0, x1);
      assert(acc_z[p] == 333 + idx);
    }
  }
}

void main_task(const Task *task,
               const std::vector<PhysicalRegion> &regions,
               Context ctx,
               Runtime *runtime)
{
  const InputArgs &command_args = Runtime::get_input_args();
  char **argv = command_args.argv;
  int argc = command_args.argc;

  TestArgs args;

  for (int i = 0; i < argc; ++i)
    if (strcmp(argv[i], "-index") == 0)
      args.index_launch = true;
    else if (strcmp(argv[i], "-predicate") == 0)
      args.predicate = true;
    else if (strcmp(argv[i], "-empty") == 0)
      args.empty = true;
    else if (strcmp(argv[i], "-replicate") == 0)
      args.replicate = true;

  Predicate pred = Predicate::TRUE_PRED;
  if (args.predicate)
  {
    TaskLauncher condition_launcher(TID_CONDITION, TaskArgument());
    Future f = runtime->execute_task(ctx, condition_launcher);
    pred = runtime->create_predicate(ctx, f);
  }

  FieldSpace fs = runtime->create_field_space(ctx);
  FieldAllocator allocator = runtime->create_field_allocator(ctx, fs);
  allocator.allocate_field(sizeof(int64_t), FID_X);
  allocator.allocate_field(sizeof(int32_t), FID_Y);
  allocator.allocate_field(sizeof(int64_t), FID_Z);
  allocator.allocate_field(0, FID_W);

  std::set<FieldID> field_set1{FID_X, FID_Y};
  // TODO: Currently we can't return a buffer to FID_W, as deferred buffers
  //       of a zero-size field cannot be created. Put back this test case
  //       once we add APIs to return untyped buffers to output regions.
  // std::set<FieldID> field_set2{FID_Z, FID_W};
  std::set<FieldID> field_set2{FID_Z};

  std::vector<OutputRequirement> out_reqs_global;
  std::vector<OutputRequirement> out_reqs_local;
  out_reqs_global.push_back(OutputRequirement(fs, field_set1, 2, true));
  out_reqs_global.back().set_type_tag<2, int32_t>();
  out_reqs_local.push_back(OutputRequirement(fs, field_set2, 2, false));
  out_reqs_local.back().set_type_tag<2, int32_t>();

  TaskArgument task_args(&args, sizeof(args));

  if (args.index_launch)
  {
    {
      Domain launch_domain(Rect<2>(Point<2>(0, 0), Point<2>(2, 2)));
      IndexTaskLauncher launcher(
        TID_PRODUCER_GLOBAL, launch_domain, task_args, ArgumentMap(), pred);
      runtime->execute_index_space(ctx, launcher, &out_reqs_global);
    }

    {
      Domain launch_domain(Rect<1>(Point<1>(0), Point<1>(2)));
      IndexTaskLauncher launcher(
        TID_PRODUCER_LOCAL, launch_domain, task_args, ArgumentMap(), pred);
      runtime->execute_index_space(ctx, launcher, &out_reqs_local);
    }
  }
  else
  {
    {
      TaskLauncher launcher(TID_PRODUCER_GLOBAL, task_args, pred);
      launcher.point = Point<2>::ZEROES();
      runtime->execute_task(ctx, launcher, &out_reqs_global);
    }

    {
      TaskLauncher launcher(TID_PRODUCER_LOCAL, task_args, pred);
      launcher.point = Point<1>::ZEROES();
      runtime->execute_task(ctx, launcher, &out_reqs_local);
    }
  }

  OutputRequirement &out_global = out_reqs_global.front();
  OutputRequirement &out_local = out_reqs_local.front();

  MappingTags tags[] = { TAG_REUSE, TAG_CREATE_NEW };

  for (unsigned i = 0; i < 2; ++i)
  {
    if (i == 0 && args.predicate) continue;

    if (args.index_launch)
    {
      {
        Domain launch_domain(Rect<2>(Point<2>(0, 0), Point<2>(2, 2)));
        IndexTaskLauncher launcher(
            TID_CONSUMER_GLOBAL, launch_domain, task_args, ArgumentMap());
        RegionRequirement req(
            out_global.partition, 0, READ_ONLY, EXCLUSIVE, out_global.parent);
        req.add_field(FID_X);
        req.add_field(FID_Y);
        req.tag = tags[i];
        launcher.add_region_requirement(req);
        runtime->execute_index_space(ctx, launcher);
      }

      {
        Domain launch_domain(Rect<1>(Point<1>(0), Point<1>(2)));
        IndexTaskLauncher launcher(
            TID_CONSUMER_LOCAL, launch_domain, task_args, ArgumentMap());
        RegionRequirement req(
            out_local.partition, 0, READ_ONLY, EXCLUSIVE, out_local.parent);
        req.add_field(FID_Z);
        req.tag = tags[i];
        launcher.add_region_requirement(req);
        runtime->execute_index_space(ctx, launcher);
      }
    }
    else
    {
      MappingTagID tag = 0;
      if (i == 0)
        tag = TAG_LOCAL_PROCESSOR;
      {
        TaskLauncher launcher(
            TID_CONSUMER_GLOBAL, task_args, Predicate::TRUE_PRED, 0, tag);
        launcher.point = Point<2>::ZEROES();
        RegionRequirement req(
            out_global.region, READ_ONLY, EXCLUSIVE, out_global.region);
        req.add_field(FID_X);
        req.add_field(FID_Y);
        req.tag = tags[i];
        launcher.add_region_requirement(req);
        runtime->execute_task(ctx, launcher);
      }
      {
        TaskLauncher launcher(
            TID_CONSUMER_LOCAL, task_args, Predicate::TRUE_PRED, 0, tag);
        launcher.point = Point<1>::ZEROES();
        RegionRequirement req(
            out_local.region, READ_ONLY, EXCLUSIVE, out_local.region);
        req.add_field(FID_Z);
        req.tag = tags[i];
        launcher.add_region_requirement(req);
        runtime->execute_task(ctx, launcher);
      }
    }
  }
}

static void create_mappers(Machine machine, Runtime *runtime, const std::set<Processor> &local_procs)
{
  for (std::set<Processor>::const_iterator it = local_procs.begin();
        it != local_procs.end(); it++)
  {
    OutReqTestMapper* mapper = new OutReqTestMapper(
      runtime->get_mapper_runtime(), machine, *it, "output_requirement_test_mapper");
    runtime->replace_default_mapper(mapper, *it);
  }
}

int main(int argc, char **argv)
{
  bool replicate = false;
  for (int i = 0; i < argc; ++i)
    if (strcmp(argv[i], "-replicate") == 0)
      replicate = true;
  {
    TaskVariantRegistrar registrar(TID_MAIN, "main");
    registrar.add_constraint(ProcessorConstraint(Processor::LOC_PROC));
    registrar.set_inner();
    registrar.set_leaf(false);
    registrar.set_inner(true);
    registrar.set_replicable(replicate);
    Runtime::preregister_task_variant<main_task>(registrar, "main");
  }
  {
    TaskVariantRegistrar registrar(TID_PRODUCER_GLOBAL, "producer_global");
    registrar.add_constraint(ProcessorConstraint(Processor::LOC_PROC));
    registrar.set_leaf(false);
    registrar.set_inner(false);
    Runtime::preregister_task_variant<producer_global_task>(registrar, "producer_global");
  }
  {
    TaskVariantRegistrar registrar(TID_PRODUCER_LOCAL, "producer_local");
    registrar.add_constraint(ProcessorConstraint(Processor::LOC_PROC));
    registrar.set_leaf(true);
    registrar.set_inner(false);
    Runtime::preregister_task_variant<producer_local_task>(registrar, "producer_local");
  }
  {
    TaskVariantRegistrar registrar(TID_CONSUMER_GLOBAL, "consumer_global");
    registrar.add_constraint(ProcessorConstraint(Processor::LOC_PROC));
    registrar.set_leaf(true);
    Runtime::preregister_task_variant<consumer_global_task>(registrar, "consumer_global");
  }
  {
    TaskVariantRegistrar registrar(TID_CONSUMER_LOCAL, "consumer_local");
    registrar.add_constraint(ProcessorConstraint(Processor::LOC_PROC));
    registrar.set_leaf(true);
    Runtime::preregister_task_variant<consumer_local_task>(registrar, "consumer_local");
  }
  {
    TaskVariantRegistrar registrar(TID_CONDITION, "condition");
    registrar.add_constraint(ProcessorConstraint(Processor::LOC_PROC));
    registrar.set_leaf(true);
    Runtime::preregister_task_variant<bool, condition_task>(registrar, "condition");
  }
  Runtime::add_registration_callback(create_mappers);

  Runtime::set_top_level_task_id(TID_MAIN);

  Runtime::start(argc, argv);

  return 0;
}
