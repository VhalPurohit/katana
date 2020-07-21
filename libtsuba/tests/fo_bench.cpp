#include <iostream>
#include <random>
#include <time.h>
#include <thread>
#include <chrono>

#include "arrow/api.h"
//#include "arrow/filesystem/api.h"
#include "parquet/file_reader.h"

#include "galois/Logging.h"
#include "galois/FileSystem.h"
#include "tsuba/Errors.h"
#include "tsuba/FileView.h"
#include "tsuba/FileFrame.h"
#include "tsuba/tsuba.h"
#include "tsuba/file.h"
#include "bench_utils.h"

/* This file is intended to benchmark a change in our arrow parquet translation.
 * *ArrLib functions represent the very first draft of that translation.
 *
 * A useful one-liner is
 * ./fo_bench | (sed -u 1q; sort)
 * It will keep the header line but sort the rest of the output, thereby
 * grouping together runs of the same test.
 */

namespace {

const int64_t BIG_ARRAY_SIZE = 1 << 27;
const int64_t NUM_RUNS       = 1;
const double NANO            = 1000000000;
const std::string s3_base    = "s3://simon-test-useast2";
const std::string local_base = "/tmp";

// Utilities
std::string write_timing_string(struct timespec before, struct timespec middle,
                                struct timespec after) {
  struct timespec total  = timespec_sub(after, before);
  struct timespec first  = timespec_sub(middle, before);
  struct timespec second = timespec_sub(after, middle);
  return fmt::format("{},{},{}", total.tv_sec + total.tv_nsec / NANO,
                     first.tv_sec + first.tv_nsec / NANO,
                     second.tv_sec + second.tv_nsec / NANO);
}

std::string read_timing_string(struct timespec before, struct timespec middle,
                               struct timespec after) {
  struct timespec total  = timespec_sub(after, before);
  struct timespec first  = timespec_sub(middle, before);
  struct timespec second = timespec_sub(after, middle);
  return fmt::format("{},{},{}", total.tv_sec + total.tv_nsec / NANO,
                     second.tv_sec + second.tv_nsec / NANO,
                     first.tv_sec + first.tv_nsec / NANO);
}

// Schemas
std::shared_ptr<arrow::Schema> int64_schema() {
  auto field = std::make_shared<arrow::Field>(
      "test", std::make_shared<arrow::Int64Type>());
  auto schema =
      std::make_shared<arrow::Schema>(arrow::Schema({field}, nullptr));
  return schema;
}

std::shared_ptr<arrow::Schema> int32_schema() {
  auto field = std::make_shared<arrow::Field>(
      "test", std::make_shared<arrow::Int32Type>());
  auto schema = std::make_shared<arrow::Schema>(arrow::Schema({field}));
  return schema;
}

// Tables
std::shared_ptr<arrow::Table> big_table() {
  arrow::Int64Builder builder;
  arrow::Status status;

  for (int64_t i = 0; i < BIG_ARRAY_SIZE; ++i) {
    status = builder.Append(i * i);
    GALOIS_LOG_ASSERT(status.ok());
  }

  std::shared_ptr<arrow::Int64Array> arr;
  status = builder.Finish(&arr);
  GALOIS_LOG_ASSERT(status.ok());
  std::shared_ptr<arrow::Table> tab = arrow::Table::Make(int64_schema(), {arr});
  return tab;
}

std::shared_ptr<arrow::Table> huge_table() {
  arrow::Int64Builder builder;
  arrow::Status status;

  for (int64_t i = 0; i < BIG_ARRAY_SIZE * 4; ++i) {
    status = builder.Append(i * i);
    GALOIS_LOG_ASSERT(status.ok());
  }

  std::shared_ptr<arrow::Int64Array> arr;
  status = builder.Finish(&arr);
  GALOIS_LOG_ASSERT(status.ok());
  std::shared_ptr<arrow::Table> tab = arrow::Table::Make(int64_schema(), {arr});
  return tab;
}

std::shared_ptr<arrow::Table> small_table() {
  arrow::Int32Builder builder;
  arrow::Status status;

  for (int32_t i = 0; i < BIG_ARRAY_SIZE; ++i) {
    status = builder.Append(i * i);
    GALOIS_LOG_ASSERT(status.ok());
  }

  std::shared_ptr<arrow::Int32Array> arr;
  status = builder.Finish(&arr);
  GALOIS_LOG_ASSERT(status.ok());
  std::shared_ptr<arrow::Table> tab = arrow::Table::Make(int32_schema(), {arr});
  return tab;
}

std::shared_ptr<arrow::Table> speckled_table() {
  arrow::Int64Builder builder;
  arrow::Status status;

  for (int64_t i = 0; i < BIG_ARRAY_SIZE; i += 2) {
    status = builder.Append(i * i);
    GALOIS_LOG_ASSERT(status.ok());
    status = builder.AppendNull();
    GALOIS_LOG_ASSERT(status.ok());
  }

  std::shared_ptr<arrow::Int64Array> arr;
  status = builder.Finish(&arr);
  GALOIS_LOG_ASSERT(status.ok());
  std::shared_ptr<arrow::Table> tab = arrow::Table::Make(int64_schema(), {arr});
  return tab;
}

std::shared_ptr<arrow::Table> super_void_table() {
  arrow::Int64Builder builder;
  arrow::Status status;

  status = builder.Append(0);
  GALOIS_LOG_ASSERT(status.ok());
  for (int64_t i = 0; i < BIG_ARRAY_SIZE - 2; ++i) {
    status = builder.AppendNull();
    GALOIS_LOG_ASSERT(status.ok());
  }
  status = builder.Append(1);
  GALOIS_LOG_ASSERT(status.ok());

  std::shared_ptr<arrow::Int64Array> arr;
  status = builder.Finish(&arr);
  GALOIS_LOG_ASSERT(status.ok());
  std::shared_ptr<arrow::Table> tab = arrow::Table::Make(int64_schema(), {arr});
  return tab;
}

std::shared_ptr<arrow::Table> please_compress_table() {
  arrow::Int64Builder builder;
  arrow::Status status;

  status = builder.Append(0);
  GALOIS_LOG_ASSERT(status.ok());
  for (int64_t i = 0; i < BIG_ARRAY_SIZE - 2; ++i) {
    status = builder.Append(34);
    GALOIS_LOG_ASSERT(status.ok());
  }
  status = builder.Append(1);
  GALOIS_LOG_ASSERT(status.ok());

  std::shared_ptr<arrow::Int64Array> arr;
  status = builder.Finish(&arr);
  GALOIS_LOG_ASSERT(status.ok());
  std::shared_ptr<arrow::Table> tab = arrow::Table::Make(int64_schema(), {arr});
  return tab;
}

// Benchmarks
void WriteArrLib(std::shared_ptr<arrow::Table> table, std::string path) {
  struct timespec start;
  struct timespec middle;
  struct timespec end;
  start = now();

  auto create_result = arrow::io::BufferOutputStream::Create();
  GALOIS_LOG_ASSERT(create_result.ok());

  std::shared_ptr<arrow::io::BufferOutputStream> out =
      create_result.ValueOrDie();
  auto write_result =
      parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out,
                                 std::numeric_limits<int64_t>::max());
  GALOIS_LOG_ASSERT(write_result.ok());

  auto finish_result = out->Finish();
  GALOIS_LOG_ASSERT(finish_result.ok());

  std::shared_ptr<arrow::Buffer> buf = finish_result.ValueOrDie();

  middle = now();

  auto res = tsuba::FileStore(path, buf->data(), buf->size());
  GALOIS_LOG_ASSERT(res);

  end = now();

  std::cout << "Arrow_Library_Write"
            << ",";
  std::cout << path << ",";
  std::cout << write_timing_string(start, middle, end) << std::endl;
}

// SCB July.13.2020 Arrow S3 support is difficult to use. It is not compiled
// into the 0.17.1 apt package, and I couldn't get it to work even after I built
// libarrow from source with S3 enabled.
//
//  std::shared_ptr<arrow::Table>
// ReadArrLib(std::string path) {
//
//  struct timespec start;
//  struct timespec middle;
//  struct timespec end;
//
//  start = now();
//
//  arrow::Status init_stat = arrow::fs::EnsureS3Initialized();
//  GALOIS_LOG_ASSERT(init_stat.ok());
//  auto s3opt = arrow::fs::S3Options::Defaults();
//  //  s3opt.region   = "us-east-2";
//  auto fs_result = arrow::fs::S3FileSystem::Make(s3opt);
//  GALOIS_LOG_ASSERT(fs_result.ok());
//
//  std::shared_ptr<arrow::fs::S3FileSystem> s3fs = fs_result.ValueOrDie();
//  auto fs =
//  std::make_shared<arrow::fs::SubTreeFileSystem>("simon-test-useast2",
//                                                           s3fs);
//  auto open_result = fs->OpenInputFile(path);
//  std::cout << open_result.status() << std::endl;
//  GALOIS_LOG_ASSERT(open_result.ok());
//
//  std::shared_ptr<arrow::io::RandomAccessFile> f = open_result.ValueOrDie();
//  std::unique_ptr<parquet::arrow::FileReader> reader;
//  auto open_file_result =
//      parquet::arrow::OpenFile(f, arrow::default_memory_pool(), &reader);
//  GALOIS_LOG_ASSERT(open_file_result.ok());
//
//  middle = now();
//
//  std::shared_ptr<arrow::Table> out;
//  auto read_result = reader->ReadTable(&out);
//  GALOIS_LOG_ASSERT(read_result.ok());
//
//  arrow::Status status = arrow::fs::FinalizeS3();
//  GALOIS_LOG_ASSERT(status.ok());
//
//  end = now();
//
//  std::cout << "Arrow_Library_Read"
//            << ",";
//  std::cout << path << ",";
//  std::cout << read_timing_string(start, middle, end) << std::endl;
//  return out;
//}

void ReadArrLib(std::string path) {
  std::cout << "Arrow_Library_Read"
            << ",";
  std::cout << path << ",";
  std::cout << "-----,-----,-----" << std::endl;
}

void WriteFF(std::shared_ptr<arrow::Table> table, std::string path) {
  struct timespec start;
  struct timespec middle;
  struct timespec end;
  start = now();

  auto ff  = std::make_shared<tsuba::FileFrame>();
  auto res = ff->Init();
  GALOIS_LOG_ASSERT(res);

  auto write_result =
      parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), ff,
                                 std::numeric_limits<int64_t>::max());
  GALOIS_LOG_ASSERT(write_result.ok());

  middle = now();

  ff->Bind(path);
  res = ff->Persist();
  GALOIS_LOG_ASSERT(res);

  end = now();

  std::cout << "FileFrame_Write"
            << ",";
  std::cout << path << ",";
  std::cout << write_timing_string(start, middle, end) << std::endl;
}

std::shared_ptr<arrow::Table> ReadFV(std::string path) {
  struct timespec start;
  struct timespec middle;
  struct timespec end;

  start = now();

  auto fv  = std::make_shared<tsuba::FileView>();
  auto res = fv->Bind(path);
  GALOIS_LOG_ASSERT(res);

  middle = now();

  std::unique_ptr<parquet::arrow::FileReader> reader;
  auto open_file_result =
      parquet::arrow::OpenFile(fv, arrow::default_memory_pool(), &reader);
  GALOIS_LOG_ASSERT(open_file_result.ok());

  std::shared_ptr<arrow::Table> out;
  auto read_result = reader->ReadTable(&out);
  GALOIS_LOG_ASSERT(read_result.ok());

  end = now();

  std::cout << "FileView_Read"
            << ",";
  std::cout << path << ",";
  std::cout << read_timing_string(start, middle, end) << std::endl;
  return out;
}

std::shared_ptr<arrow::Table> ReadPartial_v0(std::string path, int64_t offset,
                                             int64_t length) {
  GALOIS_LOG_ASSERT(offset >= 0 && length >= 0);

  struct timespec start;
  struct timespec middle;
  struct timespec end;

  start = now();

  auto fv  = std::make_shared<tsuba::FileView>(tsuba::FileView());
  auto res = fv->Bind(path);
  GALOIS_LOG_ASSERT(res);

  middle = now();

  std::unique_ptr<parquet::arrow::FileReader> reader;

  auto open_file_result =
      parquet::arrow::OpenFile(fv, arrow::default_memory_pool(), &reader);
  GALOIS_LOG_ASSERT(open_file_result.ok());

  std::shared_ptr<arrow::Table> out;
  std::shared_ptr<arrow::Table> ret;
  auto read_result = reader->ReadTable(&out);
  GALOIS_LOG_ASSERT(read_result.ok());
  ret = out->Slice(offset, length);

  end = now();

  std::cout << "FileView_Partial_Read_v0"
            << ",";
  std::cout << path << ",";
  std::cout << read_timing_string(start, middle, end) << std::endl;
  return ret;
}

std::shared_ptr<arrow::Table> ReadPartial_v1(std::string path, int64_t offset,
                                             int64_t length) {
  GALOIS_LOG_ASSERT(offset >= 0 && length >= 0);

  struct timespec start;
  struct timespec middle;
  struct timespec end;

  start = now();

  auto fv  = std::make_shared<tsuba::FileView>(tsuba::FileView());
  auto res = fv->Bind(path);
  GALOIS_LOG_ASSERT(res);

  middle = now();

  std::unique_ptr<parquet::arrow::FileReader> reader;

  auto open_file_result =
      parquet::arrow::OpenFile(fv, arrow::default_memory_pool(), &reader);
  GALOIS_LOG_ASSERT(open_file_result.ok());

  std::vector<int> row_groups;
  int rg_count            = reader->num_row_groups();
  int64_t internal_offset = 0;
  int64_t cumulative_rows = 0;
  for (int i = 0; cumulative_rows < offset + length && i < rg_count; ++i) {
    int64_t new_rows =
        reader->parquet_reader()->metadata()->RowGroup(i)->num_rows();
    if (offset < cumulative_rows + new_rows) {
      if (row_groups.empty()) {
        internal_offset = offset - cumulative_rows;
        GALOIS_LOG_ASSERT(internal_offset >= 0);
      }
      row_groups.push_back(i);
    }
    cumulative_rows += new_rows;
  }

  std::shared_ptr<arrow::Table> out;
  std::shared_ptr<arrow::Table> ret;
  auto read_result = reader->ReadRowGroups(row_groups, &out);
  GALOIS_LOG_ASSERT(read_result.ok());
  ret = out->Slice(internal_offset, length);

  end = now();

  std::cout << "FileView_Partial_Read_v1"
            << ",";
  std::cout << path << ",";
  std::cout << read_timing_string(start, middle, end) << std::endl;
  return ret;
}

// Mark these functions maybe_unused so they can be commented out in the main
// test loop below.
[[maybe_unused]] void ReadWriteArrLib(std::shared_ptr<arrow::Table> table,
                                      std::string bucket,
                                      std::string object_prefix) {
  auto path_res = galois::NewPath(bucket, object_prefix);
  GALOIS_LOG_ASSERT(path_res);
  std::string path = path_res.value();
  WriteArrLib(table, path);
  // std::shared_ptr<arrow::Table> recovered = ReadArrLib(arrow_path);
  ReadArrLib(path);

  // GALOIS_LOG_ASSERT(recovered->Equals(*table));
  std::this_thread::sleep_for(std::chrono::seconds(1));
}

[[maybe_unused]] void ReadWriteFO(std::shared_ptr<arrow::Table> table,
                                  std::string bucket,
                                  std::string object_prefix) {
  auto path_res = galois::NewPath(bucket, object_prefix);
  GALOIS_LOG_ASSERT(path_res);
  std::string path = path_res.value();
  WriteFF(table, path);
  std::shared_ptr<arrow::Table> recovered = ReadFV(path);

  GALOIS_LOG_ASSERT(recovered->Equals(*table));
  std::this_thread::sleep_for(std::chrono::seconds(1));
}

[[maybe_unused]] void WriteReadPartialFO(std::shared_ptr<arrow::Table> table,
                                         std::string bucket,
                                         std::string object_prefix,
                                         int64_t length, int64_t version) {
  GALOIS_LOG_ASSERT(length > 0);
  auto path_res = galois::NewPath(bucket, object_prefix);
  GALOIS_LOG_ASSERT(path_res);
  std::string path = path_res.value();
  WriteFF(table, path);

  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<int64_t> dist(0, table->num_rows() - length);
  int64_t offset = dist(gen);

  std::shared_ptr<arrow::Table> recovered;
  switch (version) {
  case 0:
    recovered = ReadPartial_v0(path, offset, length);
    break;
  case 1:
    recovered = ReadPartial_v1(path, offset, length);
    break;
  default:
    GALOIS_LOG_ERROR("{} not a valid version number for ReadPartial", version);
  }

  GALOIS_LOG_ASSERT(recovered->Equals(*(table->Slice(offset, length))));
  std::this_thread::sleep_for(std::chrono::seconds(1));
}
} // namespace

int main() {
  std::cout << "method,file,total,memory,persistent" << std::endl;
  std::shared_ptr<arrow::Table> big = big_table();
  std::shared_ptr<arrow::Table> hug = huge_table();
  std::shared_ptr<arrow::Table> sml = small_table();
  std::shared_ptr<arrow::Table> spd = speckled_table();
  std::shared_ptr<arrow::Table> svd = super_void_table();
  std::shared_ptr<arrow::Table> cmp = please_compress_table();

  galois::Result<void> result = tsuba::Init();
  GALOIS_LOG_ASSERT(result);

  for (int64_t i = 0; i < NUM_RUNS; ++i) {
    ReadWriteFO(big, s3_base, "big");
    ReadWriteFO(sml, s3_base, "sml");
    ReadWriteFO(spd, s3_base, "spd");
    ReadWriteFO(svd, s3_base, "svd");
    ReadWriteFO(cmp, s3_base, "cmp");
    ReadWriteArrLib(big, s3_base, "big");
    ReadWriteArrLib(sml, s3_base, "sml");
    ReadWriteArrLib(spd, s3_base, "spd");
    ReadWriteArrLib(svd, s3_base, "svd");
    ReadWriteArrLib(cmp, s3_base, "cmp");
    WriteReadPartialFO(hug, s3_base, "hug", BIG_ARRAY_SIZE / 2, 0);
    WriteReadPartialFO(hug, s3_base, "hug", BIG_ARRAY_SIZE / 2, 1);
  }

  result = tsuba::Fini();
  GALOIS_LOG_ASSERT(result);

  return 0;
}