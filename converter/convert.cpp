#include "native.h"
#include <array>
#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#include <mach-o/dyld.h>
#else
#include <openssl/sha.h>
#endif

namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;
using Key = std::array<unsigned char, 32>;
using Collection = decltype(Legacy::StateData::mLiquidities);
constexpr size_t CAPACITY = Legacy::QSWAP_MAX_POOL * Legacy::QSWAP_MAX_USER_PER_POOL;
constexpr size_t COLLECTION_OFFSET = offsetof(Legacy::StateData, mLiquidities);
constexpr unsigned long long KEY_DOMAIN = 0x31504C5041575351ULL;

void require(bool ok, const std::string& reason) { if (!ok) throw std::runtime_error(reason); }
std::string hex(const void* data, size_t n)
{
    const auto* p = static_cast<const unsigned char*>(data);
    std::string s; s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { s += "0123456789abcdef"[p[i] >> 4]; s += "0123456789abcdef"[p[i] & 15]; }
    return s;
}
std::string hex(const Key& k) { return hex(k.data(), k.size()); }
Key key(const void* p) { Key k; std::memcpy(k.data(), p, 32); return k; }
Key parseKey(const std::string& s)
{
    require(s.size() == 64, "Manifest key must contain 64 lowercase hexadecimal characters");
    Key k;
    for (size_t i = 0; i < 64; ++i) {
        const auto pos = std::string("0123456789abcdef").find(s[i]);
        require(pos != std::string::npos, "Manifest key is not lowercase hexadecimal");
        if (!(i & 1)) k[i / 2] = pos << 4; else k[i / 2] |= pos;
    }
    return k;
}
std::string sha256(const Bytes& bytes)
{
    unsigned char digest[32];
#ifdef __APPLE__
    require(bytes.size() <= UINT32_MAX, "SHA256 input too large for this build");
    CC_SHA256(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest);
#else
    SHA256(bytes.data(), bytes.size(), digest);
#endif
    return hex(digest, 32);
}
Bytes read(const fs::path& path, size_t expected = 0)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    require(bool(f), "Cannot open " + path.string());
    const auto end = f.tellg();
    require(end >= 0 && (!expected || uint64_t(end) == expected),
        "Wrong state size for " + std::string(BUILD_PROFILE) + ": " + path.string()
        + " (expected " + std::to_string(expected) + " bytes)");
    Bytes bytes(static_cast<size_t>(end)); f.seekg(0);
    if (!bytes.empty()) f.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    require(bool(f) && f.peek() == EOF, "File changed or could not be read completely: " + path.string());
    return bytes;
}
std::string quote(const std::string& s)
{
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c < 32) { out += "\\u00" + hex(&c, 1); }
        else out += c;
    }
    return out + '"';
}
Key xorKey(const Key& a, const Key& b)
{
    Key k; for (size_t i = 0; i < 32; ++i) k[i] = a[i] ^ b[i]; return k;
}
Key newKey(const Key& pool, const Key& account)
{
    unsigned char input[72]; std::memcpy(input, "QSWAPLP1", 8);
    std::memcpy(input + 8, pool.data(), 32); std::memcpy(input + 40, account.data(), 32);
    Key k; KangarooTwelve(input, sizeof(input), k.data(), k.size()); return k;
}
Key newPoolKey(const Key& oldPool, const Key& issuer)
{
    unsigned char input[48]; std::memcpy(input, "QSWAPPL1", 8);
    std::memcpy(input + 8, issuer.data(), 32); std::memcpy(input + 40, oldPool.data() + 24, 8);
    Key k; KangarooTwelve(input, sizeof(input), k.data(), k.size()); return k;
}
struct Owner { Key pool, account; };
std::map<Key, std::vector<Owner>> owners(const Bytes& text, const std::string& inputSha, size_t& count,
    std::map<Key, Key>& poolIssuers)
{
    std::istringstream stream(std::string(text.begin(), text.end())); std::string line;
    require(bool(std::getline(stream, line)) && line == "QSWAP_BINDINGS_V2\t" + inputSha,
        "Expected QSWAP_BINDINGS_V2 manifest bound to this input SHA256");
    std::map<Key, std::vector<Owner>> result; std::set<std::pair<Key, Key>> unique;
    count = 0;
    while (std::getline(stream, line)) {
        require(line.size() == 131 && line[1] == '\t' && line[66] == '\t'
            && (line[0] == 'P' || line[0] == 'L'), "Malformed binding row");
        Owner o{parseKey(line.substr(2, 64)), parseKey(line.substr(67))};
        if (line[0] == 'P') {
            require(poolIssuers.emplace(o.pool, o.account).second, "Duplicate pool issuer binding");
            require(std::memcmp(o.pool.data(), o.account.data(), 24) == 0,
                "Issuer does not match legacy pool ID");
            continue;
        }
        require(unique.emplace(o.pool, o.account).second, "Duplicate ownership pair");
        result[xorKey(o.pool, o.account)].push_back(o); ++count;
    }
    require(stream.eof(), "Ownership manifest read failed");
    return result;
}

// Inspect raw metadata first: native pov()/headIndex() trust stored pointers.
template <typename C> void validateCollection(const C& c)
{
    require(c._population <= CAPACITY, "Collection population exceeds capacity");
    std::vector<unsigned int> emptyPrefix(CAPACITY + 1, 0);
    size_t occupied = 0;
    for (size_t i = 0; i < CAPACITY; ++i) {
        auto flag = (c._povOccupationFlags[i / 32] >> (2 * (i % 32))) & 3;
        require(flag != 3, "Invalid Collection occupation flag at slot " + std::to_string(i));
        emptyPrefix[i + 1] = emptyPrefix[i] + (flag == 0);
        if (flag != 1) {
            require(c._povs[i].population == 0, "Inactive PoV has live population");
            continue; // Native tombstones deliberately retain stale pointers.
        }
        ++occupied; const auto& p = c._povs[i];
        require(p.population == 1, "Duplicate or invalid actual records under key " + hex(&p.value, 32));
        require(p.headIndex >= 0 && uint64_t(p.headIndex) < c._population
            && p.tailIndex == p.headIndex && p.bstRootIndex == p.headIndex,
            "Invalid live PoV indices at slot " + std::to_string(i));
        require(c._elements[p.headIndex].povIndex == static_cast<QPI::sint64>(i), "PoV membership mismatch");
    }
    require(occupied == c._population, "Collection occupied-slot count does not match live records");
    std::set<Key> unique;
    for (size_t i = 0; i < c._population; ++i) {
        const auto& e = c._elements[i];
        require(e.povIndex >= 0 && uint64_t(e.povIndex) < CAPACITY, "Out-of-range PoV index in record " + std::to_string(i));
        const size_t slot = e.povIndex; const auto& p = c._povs[slot];
        require(((c._povOccupationFlags[slot / 32] >> (2 * (slot % 32))) & 3) == 1
            && p.headIndex == static_cast<QPI::sint64>(i), "Record is not the sole member of its live PoV");
        require(e.priority == 0 && e.bstParentIndex == -1 && e.bstLeftIndex == -1 && e.bstRightIndex == -1,
            "Unexpected priority or tree links in record " + std::to_string(i));
        require(unique.insert(key(&p.value)).second, "Duplicate live PoV key");
        const size_t home = p.value.u64._0 & (CAPACITY - 1);
        const auto gap = home <= slot ? emptyPrefix[slot] - emptyPrefix[home]
            : emptyPrefix[CAPACITY] - emptyPrefix[home] + emptyPrefix[slot];
        require(gap == 0, "Live PoV is unreachable through the native hash table");
    }
}
struct Record { Owner owner; Key oldKey, newKey; size_t sourceIndex, poolSlot; };
struct PoolCheck { unsigned __int128 sum = 0; size_t records = 0, locks = 0; };

fs::path outputPath(const fs::path& p)
{
    require(!p.filename().empty(), "Output must name a file");
    const auto result = fs::canonical(p.parent_path().empty() ? fs::path(".") : p.parent_path()) / p.filename();
    require(!fs::exists(fs::symlink_status(result)), "Refusing existing output: " + result.string());
    return result;
}
void writeExclusive(const fs::path& path, const Bytes& bytes)
{
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    require(fd >= 0, "Cannot create exclusive staged file: " + path.string());
    try {
        size_t offset = 0;
        while (offset < bytes.size()) {
            const auto n = ::write(fd, bytes.data() + offset, bytes.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "Write failed: " + path.string()); offset += n;
        }
        require(::fsync(fd) == 0, "Cannot flush staged file: " + path.string());
        const int closeResult = ::close(fd); fd = -1;
        require(closeResult == 0, "Cannot close staged file: " + path.string());
    } catch (...) { if (fd >= 0) ::close(fd); ::unlink(path.c_str()); throw; }
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::cout << "Usage: qswap-convert INPUT_STATE OWNERS_TSV NEW_STATE REPORT_JSON\n"
                  << "Build profile: " << BUILD_PROFILE << "; inputs are read-only; existing outputs are refused.\n";
        return 0;
    }
    if (argc != 5) { std::cerr << "Usage: qswap-convert INPUT_STATE OWNERS_TSV NEW_STATE REPORT_JSON\n"; return 2; }
    fs::path stateStage, reportStage; bool ownStateStage = false, ownReportStage = false, published = false, reportPublished = false;
    fs::path output, reportPath;
    try {
        require(sizeof(void*) == 8 && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "Unsupported native ABI");
        Key a, b; for (size_t i = 0; i < 32; ++i) { a[i] = i; b[i] = i + 32; }
        require(hex(newKey(a, b)) == "a1dc06a9c122def62fc8778cee2bea2f3cce4be8d1c2754eec0a9121ab693e6b", "Native K12 self-check failed");
        const auto input = fs::canonical(argv[1]), manifest = fs::canonical(argv[2]);
        output = outputPath(argv[3]); reportPath = outputPath(argv[4]);
        require(output != reportPath && output != input && output != manifest && reportPath != input && reportPath != manifest,
            "Inputs and outputs must be distinct files");
        std::cout << "[1/6] Read original state (" << BUILD_PROFILE << ", " << sizeof(Legacy::StateData) << " bytes)" << std::endl;
        const Bytes original = read(input, sizeof(Legacy::StateData));
        const Bytes manifestBytes = read(manifest); const auto inputSha = sha256(original);
        std::map<Key, Key> poolIssuers;
        size_t candidateCount; const auto candidates = owners(manifestBytes, inputSha, candidateCount, poolIssuers);
        auto oldStorage = std::make_unique<Legacy::StateData>();
        std::memcpy(static_cast<void*>(oldStorage.get()), original.data(), original.size());
        const auto& old = *oldStorage;
        std::cout << "[2/6] Validate raw records and ownership candidates" << std::endl;
        validateCollection(old.mLiquidities);
        std::map<Key, size_t> pools; std::vector<PoolCheck> checks(Legacy::QSWAP_MAX_POOL);
        const Key zero{}; QPI::id self(13, 0, 0, 0); const Key selfKey = key(&self);
        for (size_t i = 0; i < Legacy::QSWAP_MAX_POOL; ++i) {
            const auto& p = old.mPoolBasicStates._values[i]; const auto pk = key(&p.poolID);
            if (pk == zero) {
                require(p.reservedQuAmount == 0 && p.reservedAssetAmount == 0 && p.totalLiquidity == 0
                    && p.accFeePerLPX64.low == 0 && p.accFeePerLPX64.high == 0, "Nonempty financial state in unused pool slot");
                continue;
            }
            require(pools.emplace(pk, i).second, "Duplicate pool ID");
            require(p.totalLiquidity >= 0 && p.reservedQuAmount >= 0 && p.reservedAssetAmount >= 0, "Negative pool amount");
        }
        std::map<Key, Key> newPools;
        std::set<Key> uniquePoolKeys;
        for (const auto& entry : pools) {
            const auto issuer = poolIssuers.find(entry.first);
            require(issuer != poolIssuers.end(), "Missing full issuer for pool " + hex(entry.first));
            const auto nk = newPoolKey(entry.first, issuer->second);
            require(uniquePoolKeys.insert(nk).second, "Duplicate new pool key");
            newPools.emplace(entry.first, nk);
        }
        require(poolIssuers.size() == pools.size(), "Issuer binding names a missing pool");
        std::vector<Record> records; size_t zeroCount = 0, lockCount = 0;
        std::set<Key> newKeys;
        for (size_t i = 0; i < old.mLiquidities._population; ++i) {
            const auto& e = old.mLiquidities._elements[i]; const auto ok = key(&old.mLiquidities._povs[e.povIndex].value);
            const auto match = candidates.find(ok);
            require(match != candidates.end(), "No owner candidate for actual record " + std::to_string(i) + " key=" + hex(ok));
            require(match->second.size() == 1, "Ambiguous ownership for actual record " + std::to_string(i) + " key=" + hex(ok));
            const auto owner = match->second.front(); const auto pool = pools.find(owner.pool);
            require(pool != pools.end(), "Actual record resolves to a missing pool");
            const auto& p = old.mPoolBasicStates._values[pool->second];
            require(e.value.liquidity >= 0 && e.value.feeDebtX64 <= p.accFeePerLPX64, "Invalid liquidity or fee debt");
            const auto nk = newKey(newPools.at(owner.pool), owner.account);
            require(newKeys.insert(nk).second, "Duplicate new K12 key");
            auto& check = checks[pool->second]; check.sum += static_cast<uint64_t>(e.value.liquidity); ++check.records;
            if (e.value.liquidity == 0) ++zeroCount;
            if (owner.account == selfKey) {
                require(e.value.liquidity == 1000, "Locked SELF record must hold exactly 1000 liquidity");
                ++check.locks; ++lockCount;
            }
            records.push_back({owner, ok, nk, i, pool->second});
        }
        std::cout << "[3/6] Reconcile " << records.size() << " stored records across " << pools.size() << " pools" << std::endl;
        for (const auto& entry : pools) {
            const auto slot = entry.second; const auto& p = old.mPoolBasicStates._values[slot]; const auto& c = checks[slot];
            require(c.sum == static_cast<uint64_t>(p.totalLiquidity), "Pool liquidity does not reconcile: " + hex(entry.first));
            require(p.totalLiquidity == 0 ? c.records == 0 : c.locks == 1,
                "Missing or invalid locked liquidity: " + hex(entry.first));
        }
        std::sort(records.begin(), records.end(), [](const Record& x, const Record& y) {
            return std::tie(x.owner.pool, x.owner.account) < std::tie(y.owner.pool, y.owner.account);
        });
        std::cout << "[4/6] Replace pool IDs and LP keys; preserve all financial bytes" << std::endl;
        auto destinationStorage = std::make_unique<Patched::StateData>();
        auto& destination = *destinationStorage;
        std::memcpy(static_cast<void*>(&destination), original.data(), original.size());
        for (const auto& entry : pools)
            std::memcpy(static_cast<void*>(&destination.mPoolBasicStates._values[entry.second].poolID), newPools.at(entry.first).data(), 32);
        auto& c = destination.mLiquidities; c.reset();
        for (const auto& r : records) {
            QPI::id nk; std::memcpy(static_cast<void*>(&nk), r.newKey.data(), 32);
            const auto& li = old.mLiquidities._elements[r.sourceIndex].value;
            Patched::LiquidityInfo newValue;
            std::memcpy(static_cast<void*>(&newValue), &li, sizeof(li));
            const auto index = c.add(nk, newValue, 0);
            require(index >= 0, "Native Collection insertion failed");
            std::memcpy(&c._elements[index].value, &li, sizeof(li));
        }
        Bytes converted(sizeof(Patched::StateData));
        std::memcpy(converted.data(), &destination, converted.size());
        validateCollection(c);
        require(c._population == records.size(), "Destination record count changed");
        for (size_t i = 0; i < records.size(); ++i) {
            const auto& r = records[i]; const auto& e = c._elements[i];
            require(key(&c._povs[e.povIndex].value) == r.newKey
                && std::memcmp(&e.value, &old.mLiquidities._elements[r.sourceIndex].value, sizeof(e.value)) == 0,
                "Destination key or financial record changed unexpectedly");
        }
        const auto tail = COLLECTION_OFFSET + sizeof(Collection);
        Bytes expectedPrefix(original.begin(), original.begin() + COLLECTION_OFFSET);
        for (const auto& entry : pools) {
            const auto offset = offsetof(Legacy::StateData, mPoolBasicStates) + entry.second * sizeof(Legacy::PoolBasicState);
            std::memcpy(expectedPrefix.data() + offset, newPools.at(entry.first).data(), 32);
        }
        require(std::memcmp(expectedPrefix.data(), converted.data(), COLLECTION_OFFSET) == 0
            && std::memcmp(original.data() + tail, converted.data() + tail, original.size() - tail) == 0,
            "State outside pool IDs and the LP Collection changed");
        const auto outputSha = sha256(converted);
        char executablePath[4096];
#ifdef __APPLE__
        uint32_t executablePathSize = sizeof(executablePath);
        require(_NSGetExecutablePath(executablePath, &executablePathSize) == 0, "Cannot identify converter executable");
#else
        const auto pathLength = ::readlink("/proc/self/exe", executablePath, sizeof(executablePath) - 1);
        require(pathLength > 0 && size_t(pathLength) < sizeof(executablePath) - 1, "Cannot identify converter executable");
        executablePath[pathLength] = 0;
#endif
        const auto binarySha = sha256(read(fs::canonical(executablePath)));
        std::ostringstream report;
        report << "{\n  \"format\": \"QSWAP_CONVERSION_REPORT_V2\",\n  \"result\": \"PASS\",\n"
            << "  \"profile\": " << quote(BUILD_PROFILE) << ",\n  \"source_lock_sha256\": " << quote(SOURCE_LOCK_SHA256)
            << ",\n  \"tool_source_sha256\": " << quote(TOOL_SOURCE_SHA256)
            << ",\n  \"converter_binary_sha256\": " << quote(binarySha)
            << ",\n  \"input_sha256\": " << quote(inputSha) << ",\n  \"manifest_sha256\": " << quote(sha256(manifestBytes))
            << ",\n  \"output_sha256\": " << quote(outputSha) << ",\n  \"input_bytes\": " << original.size()
            << ",\n  \"output_bytes\": " << converted.size() << ",\n  \"candidate_pairs\": " << candidateCount
            << ",\n  \"record_count\": " << records.size() << ",\n  \"locked_records\": " << lockCount
            << ",\n  \"zero_liquidity_records\": " << zeroCount << ",\n  \"financial_bytes_preserved\": true,\n"
            << "  \"ownership_basis\": \"externally reviewed manifest; XOR match checks consistency only\",\n  \"records\": [";
        for (size_t i = 0; i < records.size(); ++i) {
            const auto& r = records[i]; const auto& li = old.mLiquidities._elements[r.sourceIndex].value;
            report << (i ? ",\n" : "\n") << "    {\"pool\":" << quote(hex(r.owner.pool)) << ",\"account\":" << quote(hex(r.owner.account))
                << ",\"old_key\":" << quote(hex(r.oldKey)) << ",\"new_key\":" << quote(hex(r.newKey))
                << ",\"source_index\":" << r.sourceIndex << ",\"pool_slot\":" << r.poolSlot
                << ",\"liquidity\":" << quote(std::to_string(li.liquidity)) << ",\"fee_debt_low\":" << quote(std::to_string(li.feeDebtX64.low))
                << ",\"fee_debt_high\":" << quote(std::to_string(li.feeDebtX64.high)) << ",\"accumulated_fee\":" << quote(std::to_string(li.accumulatedFee)) << "}";
        }
        report << "\n  ],\n  \"pools\": [";
        size_t j = 0;
        for (const auto& entry : pools) {
            const auto& p = old.mPoolBasicStates._values[entry.second]; const auto& check = checks[entry.second];
            report << (j++ ? ",\n" : "\n") << "    {\"pool\":" << quote(hex(entry.first))
                << ",\"issuer\":" << quote(hex(poolIssuers.at(entry.first)))
                << ",\"new_pool\":" << quote(hex(newPools.at(entry.first))) << ",\"total_liquidity\":"
                << quote(std::to_string(p.totalLiquidity)) << ",\"records\":" << check.records << ",\"locked_records\":" << check.locks
                << ",\"reconciled\":true}";
        }
        report << "\n  ]\n}\n";
        const std::string reportText = report.str(); const Bytes reportBytes(reportText.begin(), reportText.end());
        stateStage = output.string() + ".tmp." + std::to_string(getpid());
        reportStage = reportPath.string() + ".tmp." + std::to_string(getpid());
        require(stateStage != reportPath && reportStage != output, "Staged paths overlap requested outputs");
        std::cout << "[5/6] Stage files and verify complete state readback" << std::endl;
        writeExclusive(stateStage, converted); ownStateStage = true;
        require(read(stateStage, converted.size()) == converted, "Converted state readback differs");
        writeExclusive(reportStage, reportBytes); ownReportStage = true;
        require(read(reportStage) == reportBytes, "Report readback differs");
        require(sha256(read(input, original.size())) == inputSha && read(manifest) == manifestBytes, "An input changed during conversion");
        std::cout << "[6/6] Publish verified files without overwriting anything" << std::endl;
        require(::link(stateStage.c_str(), output.c_str()) == 0, "Cannot publish converted state (existing output or filesystem error)");
        published = true;
        require(::link(reportStage.c_str(), reportPath.c_str()) == 0, "Cannot publish verification report");
        reportPublished = true;
        require(::unlink(stateStage.c_str()) == 0, "Cannot remove state staging link"); ownStateStage = false;
        require(::unlink(reportStage.c_str()) == 0, "Cannot remove report staging link"); ownReportStage = false;
        std::cout << "PASS: " << records.size() << " records preserved (" << lockCount << " locks, " << zeroCount << " zero), "
            << pools.size() << " pool totals reconciled.\nOutput SHA256: " << outputSha
            << "\nState: " << output << "\nReport: " << reportPath << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        if (published && ::unlink(output.c_str()) != 0) std::cerr << "ERROR: Could not remove incomplete publication: " << output << '\n';
        if (reportPublished && ::unlink(reportPath.c_str()) != 0) std::cerr << "ERROR: Could not remove incomplete report: " << reportPath << '\n';
        if (ownStateStage && ::unlink(stateStage.c_str()) != 0) std::cerr << "ERROR: Could not remove staging file: " << stateStage << '\n';
        if (ownReportStage && ::unlink(reportStage.c_str()) != 0) std::cerr << "ERROR: Could not remove staging file: " << reportStage << '\n';
        std::cerr << "Conversion failed; do not use any output from this run.\n";
        return 1;
    }
}
