// Tests for npk_pipeline: the building blocks, then whole runs whose output is
// checked against single-threaded decoding (npk::Decoder) of the same plan.
#include "test_support.hpp"

#include "npk/archive.hpp"
#include "npk/decoder.hpp"
#include "npk/install.hpp"
#include "npk/names.hpp"
#include "npk/pipeline/bounded_queue.hpp"
#include "npk/pipeline/byte_budget.hpp"
#include "npk/pipeline/index_writer.hpp"
#include "npk/pipeline/pipeline.hpp"
#include "npk/plan.hpp"

#include "disk_model.hpp"   // private to npk_pipeline
#include "npk/pipeline/disk_probe.hpp"

#include <atomic>
#include <fstream>
#include <iterator>
#include <chrono>
#include <map>
#include <random>
#include <thread>

using namespace test;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace pl = npk::pipeline;

namespace {

// What single-threaded decoding of the plan gives: output path -> bytes.
std::map<std::string, Bytes> expected_output(const npk::Plan& plan)
{
    std::map<std::string, Bytes> files;
    npk::Decoder decoder;
    for (const auto& group : plan.groups) {
        for (const auto& source : group.sources) {
            auto archive = npk::Archive::open(source.path);
            if (!archive) continue;
            for (const auto& planned : source.entries) {
                if (planned.disposition == npk::Disposition::skip) continue;
                auto data = decoder.read(*archive, source.entry(planned));
                if (data) files[npk::output_path(planned, *data)] = std::move(*data);
            }
        }
    }
    return files;
}

std::map<std::string, Bytes> output_tree(const fs::path& out)
{
    std::map<std::string, Bytes> files;
    if (!fs::exists(out)) return files;
    for (const auto& item : fs::recursive_directory_iterator(out)) {
        if (!item.is_regular_file()) continue;
        const std::string relative = npk::to_utf8(item.path().lexically_relative(out));
        if (relative.starts_with("_archives/")) continue;
        files[relative] = read_file(item.path());
    }
    return files;
}

struct Fixture {
    TempDir dir;
    std::optional<npk::Installation> installation;
    npk::ResourceManifest manifest;
    npk::Plan plan;

    explicit Fixture(const std::function<fs::path(const fs::path&)>& build, npk::PlanOptions options = {})
    {
        const fs::path root = build(dir.path());
        installation = *npk::Installation::open(root, npk::ArchiveSearch::scan);
        manifest = *npk::ResourceManifest::from_files(installation->resdb_files());
        npk::Planner planner(manifest, &*installation);
        plan = planner.build(installation->archives(), options);
    }
    [[nodiscard]] fs::path out() const { return dir.path() / "out"; }
};

pl::Summary run(npk::Plan& plan, pl::Options options)
{
    pl::Progress progress;
    return pl::run(plan, options, progress);
}

// A bigger installation: many small entries, some large ones, every mode.
fs::path busy_install(const fs::path& base)
{
    const fs::path root = base / "install";
    fs::create_directories(root / "neox_bin");
    std::vector<std::string> names;
    std::vector<FileSpec> ui, texture;
    const npk::Compression modes[] = {npk::Compression::none, npk::Compression::zlib, npk::Compression::lz4,
                                      npk::Compression::zstd};
    for (int i = 0; i < 600; ++i) {
        const std::string inner = "dir" + std::to_string(i % 7) + "\\f" + std::to_string(i) + ".xml";
        names.push_back("ui\\" + inner);
        const std::size_t size = (i % 50 == 0) ? 300'000 : static_cast<std::size_t>(i * 37 % 4000);
        ui.push_back({inner, random_bytes(size, static_cast<std::uint32_t>(i), i % 3 != 0), modes[i % 4], i % 5 != 0});
    }
    for (int i = 0; i < 12; ++i) {   // 1.5 MB each: larger than the small test budgets
        const std::string inner = "t" + std::to_string(i) + ".dds";
        names.push_back("model\\texture\\" + inner);
        texture.push_back({inner, random_bytes(1'500'000, 1000 + static_cast<std::uint32_t>(i)), modes[i % 4], true});
    }
    write_resdb(root / "res" / "resdb", names);
    write_npk(root / "res" / "ui.npk", ui);
    write_npk(root / "res" / "model" / "texture.npk", texture);
    // A newer layer overriding a third of the ui files.
    std::vector<FileSpec> patch;
    for (int i = 0; i < 600; i += 3) patch.push_back({ui[static_cast<std::size_t>(i)].inner_path, random_bytes(500, 5000 + static_cast<std::uint32_t>(i))});
    write_npk(root / "Documents" / "res" / "ui.npk", patch);
    return root;
}

} // namespace

// --- Building blocks -------------------------------------------------------------------

TEST(queue_is_fifo_and_drains_after_close)
{
    pl::BoundedQueue<int> queue(4);
    for (int i = 0; i < 4; ++i) CHECK(queue.push(i));
    std::thread producer([&] { CHECK(queue.push(4)); });   // blocks until there is room
    CHECK_EQ(queue.pop().value_or(-1), 0);
    producer.join();
    queue.close();
    CHECK(!queue.push(99));
    for (int i = 1; i <= 4; ++i) CHECK_EQ(queue.pop().value_or(-1), i);
    CHECK(!queue.pop().has_value());
}

TEST(queue_wait_ends_on_stop)
{
    pl::BoundedQueue<int> queue(1);
    std::stop_source stop;
    std::thread consumer([&] { CHECK(!queue.pop(stop.get_token()).has_value()); });
    std::this_thread::sleep_for(20ms);
    stop.request_stop();
    consumer.join();
}

TEST(budget_blocks_when_full_and_serves_in_order)
{
    pl::ByteBudget budget(100);
    CHECK(budget.acquire(80));
    std::atomic<int> order{0};
    int big_turn = 0, small_turn = 0;
    std::thread big([&] {
        CHECK(budget.acquire(60));
        big_turn = ++order;
        budget.release(60);
    });
    std::this_thread::sleep_for(20ms);
    std::thread small([&] {   // would fit right away (80 + 10), but must queue behind the big one
        CHECK(budget.acquire(10));
        small_turn = ++order;
        budget.release(10);
    });
    std::this_thread::sleep_for(20ms);
    CHECK_EQ(order.load(), 0);
    budget.release(80);
    big.join();
    small.join();
    CHECK_EQ(big_turn, 1);
    CHECK_EQ(small_turn, 2);
    CHECK_EQ(budget.in_use(), 0u);
}

TEST(budget_grants_an_oversized_request_when_empty)
{
    pl::ByteBudget budget(100);
    CHECK(budget.acquire(1000));   // nothing held: granted
    std::stop_source stop;
    std::thread waiter([&] { CHECK(!budget.acquire(1, stop.get_token())); });
    std::this_thread::sleep_for(20ms);
    stop.request_stop();
    waiter.join();
    budget.release(1000);
    CHECK_EQ(budget.in_use(), 0u);
}

TEST(csv_quotes_like_python)
{
    CHECK_EQ(pl::csv_row({"a", "b,c", "say \"hi\"", ""}), std::string("a,\"b,c\",\"say \"\"hi\"\"\",\r\n"));
    const auto rows = pl::parse_csv("x,\"y,z\"\r\n\"a\"\"b\",\r\n");
    CHECK_EQ(rows.size(), 2u);
    CHECK(rows[0] == std::vector<std::string>({"x", "y,z"}));
    CHECK(rows[1] == std::vector<std::string>({"a\"b", ""}));
}

// --- Whole runs --------------------------------------------------------------------------

TEST(pipeline_writes_what_single_threaded_decoding_gives)
{
    Fixture f(fake_install);
    const auto expected = expected_output(f.plan);
    pl::Options options;
    options.output_root = f.out();
    const pl::Summary summary = run(f.plan, options);
    CHECK(!summary.fatal);
    CHECK_EQ(summary.files_written, f.plan.totals.files_to_write);
    CHECK_EQ(summary.files_failed, 0u);
    CHECK(output_tree(f.out()) == expected);
    CHECK(output_tree(f.out()).at("res/ui/login/bg.png") == repeat("NEW", 50));
    CHECK(fs::exists(f.out() / "_archives" / "index.csv"));
    CHECK(fs::exists(f.out() / "_archives" / "res" / "ui.npk.csv"));
}

TEST(tiny_memory_budget_and_oversized_entries)
{
    Fixture f(busy_install);
    const auto expected = expected_output(f.plan);
    for (const std::size_t budget : {std::size_t{3}, std::size_t{256} << 10, std::size_t{4} << 20}) {
        npk::Plan plan = f.plan;   // fresh statuses
        pl::Options options;
        options.output_root = f.out() / std::to_string(budget);
        options.memory_budget = budget;
        options.decoder_threads = 4;
        const pl::Summary summary = run(plan, options);
        CHECK(!summary.fatal);
        CHECK_EQ(summary.files_failed, 0u);
        CHECK_EQ(summary.files_written, f.plan.totals.files_to_write);
        CHECK(output_tree(options.output_root) == expected);
    }
}

TEST(any_number_of_writers_gives_the_same_files)
{
    Fixture f(busy_install);
    const auto expected = expected_output(f.plan);
    for (const unsigned writers : {1u, 3u, 32u}) {
        npk::Plan plan = f.plan;
        pl::Options options;
        options.output_root = f.out() / std::to_string(writers);
        options.initial_writers = writers;
        options.adaptive = false;
        const pl::Summary summary = run(plan, options);
        CHECK_EQ(summary.writers, writers);
        CHECK(output_tree(options.output_root) == expected);
    }
}

TEST(entries_sharing_stored_bytes_decode_independently)
{
    // Two TOC rows pointing at the same encrypted bytes (as a deduplicating packer
    // would write them), for an LZ4 entry and for a stored one. Decrypting in place
    // would corrupt the twin.
    TempDir dir;
    const fs::path root = dir.path() / "install";
    fs::create_directories(root / "neox_bin");
    const Bytes lz4_content = random_bytes(5000, 1);
    const Bytes raw_content = random_bytes(3000, 2);
    Bytes npk = build_npk({{"a.bin", lz4_content, npk::Compression::lz4, true},
                           {"b.bin", raw_content, npk::Compression::none, true},
                           {"c.bin", bytes("tail"), npk::Compression::none, false}});
    // Duplicate rows a -> a2 and b -> b2: same offset, size and checksums, other id.
    const std::uint32_t count = 3;
    const std::size_t toc = npk.size() - count * 28;
    Bytes rows(npk.begin() + static_cast<std::ptrdiff_t>(toc), npk.end());
    Bytes extra;
    for (std::uint32_t r = 0; r < count; ++r) {
        Bytes row(rows.begin() + r * 28, rows.begin() + (r + 1) * 28);
        std::uint32_t id;
        std::memcpy(&id, row.data(), 4);
        const char* twin = id == npk::path_id("a.bin") ? "a2.bin" : id == npk::path_id("b.bin") ? "b2.bin" : nullptr;
        if (!twin) continue;
        const std::uint32_t twin_id = npk::path_id(twin);
        std::memcpy(row.data(), &twin_id, 4);
        extra.insert(extra.end(), row.begin(), row.end());
    }
    npk.insert(npk.end(), extra.begin(), extra.end());
    const std::uint32_t new_count = count + 2;
    std::memcpy(npk.data() + 4, &new_count, 4);
    write_file(root / "res" / "x.npk", npk);
    write_resdb(root / "res" / "resdb", {"x\\a.bin", "x\\b.bin", "x\\c.bin", "x\\a2.bin", "x\\b2.bin"});

    const auto installation = *npk::Installation::open(root, npk::ArchiveSearch::scan);
    const auto manifest = *npk::ResourceManifest::from_files(installation.resdb_files());
    npk::Planner planner(manifest, &installation);
    npk::Plan plan = planner.build(installation.archives(), {});
    pl::Options options;
    options.output_root = dir.path() / "out";
    const pl::Summary summary = run(plan, options);
    CHECK_EQ(summary.files_failed, 0u);
    const auto files = output_tree(options.output_root);
    CHECK(files.at("res/x/a.bin") == lz4_content);
    CHECK(files.at("res/x/a2.bin") == lz4_content);
    CHECK(files.at("res/x/b.bin") == raw_content);
    CHECK(files.at("res/x/b2.bin") == raw_content);
    CHECK(files.at("res/x/c.bin") == bytes("tail"));
}

TEST(damaged_entries_fail_alone)
{
    TempDir dir;
    const fs::path root = dir.path() / "install";
    fs::create_directories(root / "neox_bin");
    Bytes npk = build_npk({{"good.txt", repeat("good", 100)}, {"bad.txt", repeat("bad", 100)}, {"raw.txt", repeat("r", 99), npk::Compression::none, false}});
    // Damage the stored bytes of "bad.txt": find its row and flip a byte inside its data.
    const std::uint32_t count = 3;
    const std::size_t toc = npk.size() - count * 28;
    for (std::uint32_t r = 0; r < count; ++r) {
        std::uint32_t id, offset;
        std::memcpy(&id, npk.data() + toc + r * 28, 4);
        std::memcpy(&offset, npk.data() + toc + r * 28 + 4, 4);
        if (id == npk::path_id("bad.txt")) npk[offset + 3] ^= std::byte{0x40};
    }
    write_file(root / "res" / "x.npk", npk);
    write_resdb(root / "res" / "resdb", {"x\\good.txt", "x\\bad.txt", "x\\raw.txt"});

    const auto installation = *npk::Installation::open(root, npk::ArchiveSearch::scan);
    const auto manifest = *npk::ResourceManifest::from_files(installation.resdb_files());
    npk::Planner planner(manifest, &installation);
    npk::Plan plan = planner.build(installation.archives(), {});
    pl::Options options;
    options.output_root = dir.path() / "out";
    const pl::Summary summary = run(plan, options);
    CHECK_EQ(summary.files_written, 2u);
    CHECK_EQ(summary.files_failed, 1u);
    CHECK(!summary.fatal);
    const auto files = output_tree(options.output_root);
    CHECK(files.contains("res/x/good.txt") && files.contains("res/x/raw.txt") && !files.contains("res/x/bad.txt"));
    const std::string record = [&] {
        const Bytes b = read_file(options.output_root / "_archives" / "res" / "x.npk.csv");
        return std::string(reinterpret_cast<const char*>(b.data()), b.size());
    }();
    CHECK(record.find("failed,,stored data is damaged (crc_packed does not match)") != std::string::npos);

    // Without verification the damage goes unnoticed or fails in the codec; never a crash.
    npk::Plan again = planner.build(installation.archives(), {});
    options.output_root = dir.path() / "out2";
    options.verify = false;
    (void)run(again, options);
}

TEST(unusable_output_folder_is_fatal)
{
    Fixture f(fake_install);
    write_file(f.dir.path() / "file", bytes("x"));   // the output "folder" is a file
    pl::Options options;
    options.output_root = f.dir.path() / "file" / "out";
    const pl::Summary summary = run(f.plan, options);
    CHECK(summary.fatal.has_value());
    CHECK_EQ(summary.files_written, 0u);
    CHECK_EQ(summary.files_not_reached, f.plan.totals.files_to_write);
}

TEST(cancelled_run_stops_and_records_what_was_done)
{
    Fixture f(busy_install);
    pl::Options options;
    options.output_root = f.out();
    pl::Progress progress;
    std::stop_source cancel;
    cancel.request_stop();   // before it even starts
    const pl::Summary summary = pl::run(f.plan, options, progress, cancel.get_token());
    CHECK(summary.cancelled);
    CHECK_EQ(summary.files_written + summary.files_failed + summary.files_not_reached, f.plan.totals.files_to_write);
    CHECK(summary.files_not_reached > 0);
    CHECK(fs::exists(f.out() / "_archives" / "index.csv"));
}

TEST(a_second_run_keeps_earlier_index_rows)
{
    TempDir dir;
    const fs::path root = fake_install(dir.path());
    const auto installation = *npk::Installation::open(root, npk::ArchiveSearch::scan);
    const auto manifest = *npk::ResourceManifest::from_files(installation.resdb_files());
    npk::Planner planner(manifest, &installation);
    pl::Options options;
    options.output_root = dir.path() / "out";

    const std::vector<fs::path> first{root / "res" / "ui.npk"};
    npk::Plan plan_a = planner.build(first, {});
    (void)run(plan_a, options);
    const std::vector<fs::path> second{root / "script.npk"};
    npk::Plan plan_b = planner.build(second, {});
    (void)run(plan_b, options);

    const Bytes index = read_file(options.output_root / "_archives" / "index.csv");
    const auto rows = pl::parse_csv(std::string_view(reinterpret_cast<const char*>(index.data()), index.size()));
    CHECK_EQ(rows.size(), 3u);   // header + both archives
}

// --- DiskModel ------------------------------------------------------------------------

namespace {

// Feeds windows of a known disk: seconds = a[W] * files + b * written + c * read,
// with mixes from tiny files to textures, and an occasional stall.
struct FakeTimes {
    std::map<unsigned, double> a;   // seconds per file at W writers
    double b = 1 / 150e6;           // seconds per byte written
    double c = 0;                   // seconds per byte read
    std::mt19937 random{7};
    std::int64_t now = 0;

    pl::Window window(double kb_per_file)
    {
        (void)this;
        pl::Window w;
        w.files = std::uniform_real_distribution<double>(50, 400)(random);
        w.bytes_written = w.files * kb_per_file * 1024;
        w.bytes_read = w.bytes_written / 2;
        return w;
    }
    void feed(pl::DiskModel& model, unsigned writers, double kb_per_file, int windows)
    {
        for (int i = 0; i < windows; ++i) {
            pl::Window w = window(kb_per_file * std::uniform_real_distribution<double>(0.5, 1.5)(random));
            w.seconds = a.at(writers) * w.files + b * w.bytes_written + c * w.bytes_read;
            if (std::uniform_int_distribution<int>(0, 19)(random) == 0) w.seconds += 0.3;   // stall
            now += 500'000'000;
            model.observe_mix(w);
            model.observe(writers, w, now);
        }
    }
};

bool near(double value, double expected, double tolerance) { return std::abs(value / expected - 1) <= tolerance; }

} // namespace

TEST(disk_model_recovers_the_costs)
{
    FakeTimes disk;
    disk.a = {{1, 600e-6}, {2, 1100e-6}};
    disk.c = 1 / 300e6;
    pl::DiskModel model;
    // Alternate counts and mixes, as a run would.
    for (int round = 0; round < 6; ++round) {
        disk.feed(model, 1, round % 2 ? 300 : 8, 40);
        model.break_chunk();
        disk.feed(model, 2, round % 3 ? 40 : 600, 40);
        model.break_chunk();
    }
    // Reads here are always half the bytes written, so only the combined byte cost can be known.
    CHECK(near(model.seconds_per_byte_written() + model.seconds_per_byte_read() / 2, disk.b + disk.c / 2, 0.15));
    const auto one = model.estimate(1, disk.now);
    const auto two = model.estimate(2, disk.now);
    CHECK(one && two);
    CHECK(near(one->per_file, 600e-6, 0.15));
    CHECK(near(two->per_file, 1100e-6, 0.15));
    CHECK(model.known(1, disk.now) && model.known(2, disk.now));
    CHECK(!model.known(3, disk.now));
}

// The case that fooled the throughput-based tuner: the better count happened to
// be measured on tiny files, the worse one on big files that score well.
TEST(disk_model_ranks_counts_whatever_mix_they_were_measured_on)
{
    FakeTimes disk;
    disk.a = {{1, 600e-6}, {2, 1100e-6}};
    pl::DiskModel model;
    disk.feed(model, 1, 4, 40);       // tiny files at 1 writer: little written per second
    model.break_chunk();
    disk.feed(model, 1, 200, 40);
    model.break_chunk();
    disk.feed(model, 2, 600, 40);     // big files at 2 writers: many bytes per second
    model.break_chunk();
    disk.feed(model, 2, 60, 40);
    const auto one = model.predict(1, disk.now);
    const auto two = model.predict(2, disk.now);
    CHECK(one && two);
    CHECK(one->seconds < two->seconds);
}

TEST(a_second_run_writes_only_what_is_missing)
{
    Fixture f(busy_install);
    pl::Options options;
    options.output_root = f.out();
    options.disk_kind = pl::DiskKind::ssd;
    npk::Plan first = f.plan;
    (void)run(first, options);
    const auto expected = output_tree(f.out());
    const auto read_index = [&] {
        std::ifstream in(f.out() / "_archives" / "res" / "ui.npk.csv", std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    const std::string index = read_index();

    // Lose a few files and cut one short, as an interrupted run would.
    std::size_t removed = 0;
    for (const auto& entry : fs::recursive_directory_iterator(f.out() / "res" / "ui" / "dir3")) {
        if (entry.is_regular_file() && removed++ < 5) fs::remove(entry.path());
    }
    fs::resize_file(f.out() / "res" / "model" / "texture" / "t0.dds", 1000);

    const npk::ExistingFiles existing = npk::ExistingFiles::scan(f.out());
    npk::PlanOptions plan_options;
    plan_options.existing = &existing;
    npk::Planner planner(f.manifest, &*f.installation);
    npk::Plan second = planner.build(f.installation->archives(), plan_options);
    CHECK_EQ(second.totals.files_to_write, 6u);

    pl::Progress progress(second.source_count());
    const pl::Summary summary = pl::run(second, options, progress);
    CHECK_EQ(summary.files_written, 6u);
    CHECK_EQ(summary.files_existing, first.totals.files_to_write - 6);
    CHECK(output_tree(f.out()) == expected);
    CHECK(read_index() == index);   // the kept files are recorded as before

    std::uint64_t done = 0;
    for (const auto& counts : progress.sources()) done += counts.files_done.load();
    CHECK_EQ(done, 6u);
}

TEST(disk_probe_tells_hard_disks_from_ssds)
{
    CHECK(pl::classify(0) == pl::DiskKind::unknown);
    CHECK(pl::classify(0.08) == pl::DiskKind::ssd);    // NVMe
    CHECK(pl::classify(0.3) == pl::DiskKind::ssd);     // a slow SATA SSD
    CHECK(pl::classify(2.5) == pl::DiskKind::hdd);     // a 15k rpm disk at best
    CHECK(pl::classify(9.0) == pl::DiskKind::hdd);

    // A real probe of the temporary folder decides on something and cleans up.
    TempDir dir;
    const pl::DiskProbe probe = pl::probe_disk(dir.path(), {});
    CHECK(probe.kind != pl::DiskKind::unknown);
    CHECK(probe.reads > 0);
    CHECK(fs::is_empty(dir.path()));
}

TEST(each_kind_of_disk_starts_at_its_own_writer_count)
{
    Fixture f(busy_install);
    for (const auto& [kind, writers] : {std::pair{pl::DiskKind::hdd, 1u}, std::pair{pl::DiskKind::ssd, 4u}}) {
        npk::Plan plan = f.plan;
        pl::Options options;
        options.output_root = f.out() / pl::name_of(kind);
        options.disk_kind = kind;
        options.adaptive = false;
        const pl::Summary summary = run(plan, options);
        CHECK(summary.disk == kind);
        CHECK_EQ(summary.writers, writers);
    }
}

int main()
{
    return test::run_all();
}
