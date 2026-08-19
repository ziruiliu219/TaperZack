#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#define XXH_INLINE_ALL
#include "xxhash.h"
#include "column_marshaller.h"

struct Scenario {
    const char* name;
    size_t numBatches;
    size_t batchRows;
    size_t numStrCols;
    size_t numIntCols;
    uint64_t globalCardinality;
    uint64_t newKeyRatePpm;
    size_t stringLen;
    uint64_t seed;
    size_t initialSlotCapacity;
};

struct Batch {
    std::vector<std::vector<std::vector<uint8_t>>> strCols;
    std::vector<std::vector<int64_t>> intCols;
    std::vector<int64_t> hashes;
    std::vector<int64_t> values;
    std::vector<std::vector<const uint8_t*>> strPtrs;
    std::vector<std::vector<size_t>> strLens;
};

struct Workload {
    Scenario scenario;
    std::vector<Batch> batches;
    size_t expectedGroups;
    int64_t expectedChecksum;
};

static inline uint64_t SplitMix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static std::vector<uint8_t> MakeString(uint64_t keyId, size_t col, size_t len) {
    uint64_t salt = SplitMix64(keyId ^ (static_cast<uint64_t>(col) << 32));
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << salt
        << "_k" << std::setw(16) << keyId << "_c" << std::dec << col;
    std::string s = out.str();
    while (s.size() < len) {
        std::ostringstream suffix;
        suffix << "_" << std::hex << std::setfill('0') << std::setw(16) << salt;
        s += suffix.str();
        salt = SplitMix64(salt);
    }
    s.resize(len);
    return std::vector<uint8_t>(s.begin(), s.end());
}

static inline uint64_t Rotl64(uint64_t value, unsigned bits) {
    return (value << bits) | (value >> (64 - bits));
}

static inline int64_t ValueForRow(uint64_t keyId, uint64_t globalRow) {
    return static_cast<int64_t>(SplitMix64(keyId ^ Rotl64(globalRow, 17)) % 997) + 1;
}

static uint64_t LogicalKeyId(const Scenario& s, uint64_t globalRow) {
    uint64_t draw = SplitMix64(s.seed ^ globalRow);
    if (draw % 1000000ULL < s.newKeyRatePpm) {
        return s.globalCardinality + globalRow;
    }
    return SplitMix64(s.seed + globalRow * 0xd1b54a32d192ed03ULL) % s.globalCardinality;
}

static uint64_t HashI64(uint64_t seed, int64_t value) {
    uint8_t bytes[8];
    uint64_t raw = static_cast<uint64_t>(value);
    for (int i = 0; i < 8; i++) {
        bytes[i] = static_cast<uint8_t>((raw >> (i * 8)) & 0xFF);
    }
    return XXH3_64bits_withSeed(bytes, sizeof(bytes), seed);
}

static Workload GenerateWorkload(const Scenario& s) {
    Workload w;
    w.scenario = s;
    w.expectedChecksum = 0;
    std::unordered_set<uint64_t> distinct;
    distinct.reserve(s.globalCardinality + s.numBatches * s.batchRows / 4);
    w.batches.reserve(s.numBatches);

    for (size_t batchIdx = 0; batchIdx < s.numBatches; batchIdx++) {
        Batch b;
        b.strCols.resize(s.numStrCols);
        b.intCols.resize(s.numIntCols);
        b.strPtrs.resize(s.numStrCols);
        b.strLens.resize(s.numStrCols);
        for (auto& col : b.strCols) col.reserve(s.batchRows);
        for (auto& col : b.intCols) col.reserve(s.batchRows);
        b.hashes.reserve(s.batchRows);
        b.values.reserve(s.batchRows);

        for (size_t rowIdx = 0; rowIdx < s.batchRows; rowIdx++) {
            uint64_t globalRow = static_cast<uint64_t>(batchIdx * s.batchRows + rowIdx);
            uint64_t keyId = LogicalKeyId(s, globalRow);
            distinct.insert(keyId);

            uint64_t hash = 0;
            for (size_t col = 0; col < s.numStrCols; col++) {
                auto bytes = MakeString(keyId, col, s.stringLen);
                hash = XXH3_64bits_withSeed(bytes.data(), bytes.size(), hash);
                b.strCols[col].push_back(std::move(bytes));
            }
            for (size_t col = 0; col < s.numIntCols; col++) {
                int64_t value = static_cast<int64_t>(keyId) * (97 + static_cast<int64_t>(col) * 31) + 1;
                hash = HashI64(hash, value);
                b.intCols[col].push_back(value);
            }

            int64_t aggValue = ValueForRow(keyId, globalRow);
            w.expectedChecksum += aggValue;
            b.hashes.push_back(static_cast<int64_t>(hash));
            b.values.push_back(aggValue);
        }

        for (size_t col = 0; col < s.numStrCols; col++) {
            b.strPtrs[col].resize(s.batchRows);
            b.strLens[col].resize(s.batchRows);
            for (size_t row = 0; row < s.batchRows; row++) {
                b.strPtrs[col][row] = b.strCols[col][row].data();
                b.strLens[col][row] = b.strCols[col][row].size();
            }
        }

        w.batches.push_back(std::move(b));
    }

    w.expectedGroups = distinct.size();
    return w;
}

static size_t InitialChunks(const Workload& w) {
    if (w.scenario.initialSlotCapacity > 0) {
        size_t chunks = std::max<size_t>((w.scenario.initialSlotCapacity + 7) / 8, 1);
        size_t pow2 = 1;
        while (pow2 < chunks) pow2 <<= 1;
        return pow2;
    }
    size_t minSlots = std::max(w.expectedGroups * 2, w.scenario.batchRows);
    size_t chunks = std::max<size_t>((minSlots + 7) / 8, 1);
    size_t pow2 = 1;
    while (pow2 < chunks) pow2 <<= 1;
    return pow2;
}

static std::string ScenarioParam(const Scenario& s) {
    std::string name = s.name;
    if (s.initialSlotCapacity != 0 && name == "2str_short_mostly_new") {
        name = "2str_short_reuse_small";
    }
    std::string cap;
    if (s.initialSlotCapacity == 0) {
        cap = "auto";
    } else if (s.initialSlotCapacity % 1024 == 0) {
        cap = std::to_string(s.initialSlotCapacity / 1024) + "k";
    } else {
        cap = std::to_string(s.initialSlotCapacity);
    }
    if (s.initialSlotCapacity == 0) {
        return name + "_cap=" + cap;
    }
    return name + "_cap=" + cap
        + "_card=" + std::to_string(s.globalCardinality)
        + "_newppm=" + std::to_string(s.newKeyRatePpm);
}

static std::pair<size_t, int64_t> RunTaper(const Workload& w) {
    const auto& s = w.scenario;
    taper::SimpleArenaAllocator pool;
    std::vector<taper::ColumnDesc> descs;
    descs.reserve(s.numStrCols + s.numIntCols);
    for (size_t i = 0; i < s.numStrCols; i++) descs.push_back(taper::ColumnDesc::Varchar);
    for (size_t i = 0; i < s.numIntCols; i++) descs.push_back(taper::ColumnDesc::Int64);

    taper::TaperColumnSerializeHandler table(pool, 8, descs, InitialChunks(w));

    for (const auto& b : w.batches) {
        std::vector<taper::ColumnInput> columns;
        columns.reserve(descs.size());
        for (size_t col = 0; col < s.numStrCols; col++) {
            columns.push_back(taper::ColumnInput::MakeVarchar(b.strPtrs[col].data(), b.strLens[col].data()));
        }
        for (size_t col = 0; col < s.numIntCols; col++) {
            columns.push_back(taper::ColumnInput::MakeInt64(b.intCols[col].data()));
        }

        table.EmplaceTableWithDecode(b.hashes.data(), static_cast<int32_t>(s.batchRows), columns, b.values.data());
    }

    return {table.NumGroups(), table.AggregateI64Checksum()};
}

static std::vector<Scenario> Scenarios() {
    std::vector<Scenario> base = {
        {"2str_long_reuse", 16, 16384, 2, 0, 65536, 80000, 32, 0x1234, 0},
        {"4str_long_reuse", 16, 16384, 4, 0, 65536, 80000, 32, 0x2234, 0},
        {"2str_2int_reuse", 16, 16384, 2, 2, 65536, 80000, 32, 0x3234, 0},
        {"2str_short_mostly_new", 16, 16384, 2, 0, 65536, 750000, 8, 0x4234, 0},
    };
    std::vector<Scenario> out;
    out.reserve(base.size() * 3);
    for (auto baseScenario : base) {
        for (size_t initialSlotCapacity : {static_cast<size_t>(16384), static_cast<size_t>(65536), static_cast<size_t>(0)}) {
            auto scenario = baseScenario;
            scenario.initialSlotCapacity = initialSlotCapacity;
            if (scenario.initialSlotCapacity != 0) {
                uint64_t targetGroups = static_cast<uint64_t>(static_cast<double>(scenario.initialSlotCapacity) * 0.80);
                uint64_t reuseGroups = std::max<uint64_t>(targetGroups / 2, 1);
                uint64_t uniqueNewGroups = targetGroups - reuseGroups;
                uint64_t totalRows = static_cast<uint64_t>(scenario.numBatches * scenario.batchRows);

                scenario.globalCardinality = reuseGroups;
                scenario.newKeyRatePpm = std::min<uint64_t>((uniqueNewGroups * 1000000ULL) / totalRows, 999999ULL);
            }
            out.push_back(scenario);
        }
    }
    return out;
}

static const Workload& GetWorkload(int64_t idx) {
    static std::vector<Workload> workloads;
    if (workloads.empty()) {
        for (const auto& scenario : Scenarios()) {
            workloads.push_back(GenerateWorkload(scenario));
        }
    }
    return workloads[static_cast<size_t>(idx)];
}

static void BM_TaperValidation(benchmark::State& state) {
    const auto& w = GetWorkload(state.range(0));
    auto verified = RunTaper(w);
    if (verified.first != w.expectedGroups || verified.second != w.expectedChecksum) {
        std::ostringstream err;
        err << "group count or checksum mismatch: groups actual=" << verified.first
            << " expected=" << w.expectedGroups
            << ", checksum actual=" << verified.second
            << " expected=" << w.expectedChecksum;
        state.SkipWithError(err.str().c_str());
        return;
    }

    for (auto _ : state) {
        auto result = RunTaper(w);
        benchmark::DoNotOptimize(result.first);
        benchmark::DoNotOptimize(result.second);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(w.scenario.numBatches * w.scenario.batchRows));
}

int main(int argc, char** argv) {
    auto scenarios = Scenarios();
    for (size_t i = 0; i < scenarios.size(); i++) {
        std::string name = std::string("taper_validation_cpp/") + ScenarioParam(scenarios[i]);
        benchmark::RegisterBenchmark(name.c_str(), BM_TaperValidation)->Arg(static_cast<int64_t>(i))->Iterations(10);
    }
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
}
