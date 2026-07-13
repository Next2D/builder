// V8 バイトコードキャッシュ。
//
// player+framework のバンドル (数 MB) は毎起動フルパースがかかり、jitless
// (Ignition) では起動時間の主要因になる。初回起動時に eager コンパイルして
// バイトコードをディスクへ保存し、2 回目以降はそれを消費してパースを省略する。
// 生成物はデータであり動的コード生成ではないため、コンソール (JIT 禁止) でも使える。
//
// - 保存先: %LOCALAPPDATA%\Next2D\codecache\<hash>.v8bc
//   (DomShims の localStorage バックエンドと同じ配置方針。環境変数が無ければ無効化)
// - キー: スクリプト名 + ソース内容の FNV-1a。ソースが変われば別ファイルになる。
// - V8 のバージョン/フラグ不一致は V8 自身が検出して reject する (その場で作り直す)。
// - すべてのエラーは graceful degrade (キャッシュ無しの通常コンパイルに落ちる)。
// - 小さいスクリプトはパースが安く I/O の方が高いため対象外 (kMinSourceSize)。
#pragma once

#include <v8.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace next2d::codecache {

// キャッシュ対象の最小ソースサイズ。bootstrap.js (十数 KB) は対象外、
// ゲーム/player バンドル (数百 KB〜数 MB) が対象になる境界。
inline constexpr size_t kMinSourceSize = 64 * 1024;

inline const std::filesystem::path& CacheDir()
{
    static const std::filesystem::path dir = []() -> std::filesystem::path {
        const char* base = std::getenv("LOCALAPPDATA");
        if (!base || !*base) {
            return {};
        }
        std::error_code ec;
        std::filesystem::path d = std::filesystem::path(base) / "Next2D" / "codecache";
        std::filesystem::create_directories(d, ec);
        if (ec) {
            return {};
        }
        // ゲーム更新でソースが変わるとキャッシュはファイル名ごと変わり、旧ファイルは
        // 参照されない孤児になる。30 日アクセスの無い .v8bc を起動時に掃除する
        // (Load が使用時に write time を touch するため実質 LRU)。
        const auto now = std::filesystem::file_time_type::clock::now();
        std::error_code iter_ec;
        for (const auto& entry : std::filesystem::directory_iterator(d, iter_ec)) {
            std::error_code e2;
            if (entry.path().extension() != ".v8bc") {
                continue;
            }
            const auto t = std::filesystem::last_write_time(entry.path(), e2);
            if (!e2 && now - t > std::chrono::hours(24 * 30)) {
                std::filesystem::remove(entry.path(), e2);
            }
        }
        return d;
    }();
    return dir;
}

inline uint64_t Fnv1a(const void* data, size_t size, uint64_t hash = 1469598103934665603ull)
{
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= p[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

inline std::filesystem::path CacheFileFor(const std::string& name, const std::string& source)
{
    const std::filesystem::path& dir = CacheDir();
    if (dir.empty()) {
        return {};
    }
    uint64_t h = Fnv1a(name.data(), name.size());
    h = Fnv1a(source.data(), source.size(), h);
    char buf[17] = {};
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return dir / (std::string(buf) + ".v8bc");
}

inline bool Eligible(const std::string& source)
{
    return source.size() >= kMinSourceSize && !CacheDir().empty();
}

// キャッシュファイルを読む。戻り値の所有権は ScriptCompiler::Source が取る
// (BufferOwned なので CachedData 破棄時に delete[] される)。無ければ nullptr。
inline v8::ScriptCompiler::CachedData* Load(const std::string& name, const std::string& source)
{
    const std::filesystem::path file = CacheFileFor(name, source);
    if (file.empty()) {
        return nullptr;
    }
    std::ifstream ifs(file, std::ios::binary);
    if (!ifs) {
        return nullptr;
    }
    std::vector<char> buf(
        (std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (buf.empty()) {
        return nullptr;
    }
    // 使用時に write time を更新し、CacheDir の 30 日掃除を LRU として機能させる
    std::error_code ec;
    std::filesystem::last_write_time(
        file, std::filesystem::file_time_type::clock::now(), ec);

    auto* data = new uint8_t[buf.size()];
    std::memcpy(data, buf.data(), buf.size());
    return new v8::ScriptCompiler::CachedData(
        data, static_cast<int>(buf.size()),
        v8::ScriptCompiler::CachedData::BufferOwned);
}

inline void Store(const std::string& name, const std::string& source,
                  const v8::ScriptCompiler::CachedData* data)
{
    if (!data || !data->data || data->length <= 0) {
        return;
    }
    const std::filesystem::path file = CacheFileFor(name, source);
    if (file.empty()) {
        return;
    }
    std::ofstream ofs(file, std::ios::binary | std::ios::trunc);
    if (ofs) {
        ofs.write(reinterpret_cast<const char*>(data->data), data->length);
    }
}

// classic script をキャッシュ込みでコンパイルする。
// - キャッシュ有り: kConsumeCodeCache で消費 (V8 が reject したら作り直して保存)
// - キャッシュ無し: kEagerCompile で全関数を一括コンパイルして保存
//   (初回はその分遅いが、以降の起動は lazy compile も含めて丸ごと省ける)
// - 対象外サイズ: 従来どおりの lazy コンパイル
inline v8::MaybeLocal<v8::Script> Compile(v8::Local<v8::Context> ctx,
                                          v8::Local<v8::String> src,
                                          v8::ScriptOrigin& origin,
                                          const std::string& name,
                                          const std::string& source_str)
{
    if (!Eligible(source_str)) {
        v8::ScriptCompiler::Source source(src, origin);
        return v8::ScriptCompiler::Compile(ctx, &source);
    }

    if (v8::ScriptCompiler::CachedData* cached = Load(name, source_str)) {
        v8::ScriptCompiler::Source source(src, origin, cached);
        v8::Local<v8::Script> script;
        if (!v8::ScriptCompiler::Compile(
                ctx, &source, v8::ScriptCompiler::kConsumeCodeCache).ToLocal(&script)) {
            return {};
        }
        if (source.GetCachedData() && source.GetCachedData()->rejected) {
            std::unique_ptr<v8::ScriptCompiler::CachedData> fresh(
                v8::ScriptCompiler::CreateCodeCache(script->GetUnboundScript()));
            Store(name, source_str, fresh.get());
        }
        return script;
    }

    v8::ScriptCompiler::Source source(src, origin);
    v8::Local<v8::Script> script;
    if (!v8::ScriptCompiler::Compile(
            ctx, &source, v8::ScriptCompiler::kEagerCompile).ToLocal(&script)) {
        return {};
    }
    std::unique_ptr<v8::ScriptCompiler::CachedData> fresh(
        v8::ScriptCompiler::CreateCodeCache(script->GetUnboundScript()));
    Store(name, source_str, fresh.get());
    return script;
}

// ES module 版。規約は Compile と同じ。
inline v8::MaybeLocal<v8::Module> CompileModule(v8::Isolate* isolate,
                                                v8::Local<v8::String> src,
                                                v8::ScriptOrigin& origin,
                                                const std::string& name,
                                                const std::string& source_str)
{
    if (!Eligible(source_str)) {
        v8::ScriptCompiler::Source source(src, origin);
        return v8::ScriptCompiler::CompileModule(isolate, &source);
    }

    if (v8::ScriptCompiler::CachedData* cached = Load(name, source_str)) {
        v8::ScriptCompiler::Source source(src, origin, cached);
        v8::Local<v8::Module> module;
        if (!v8::ScriptCompiler::CompileModule(
                isolate, &source, v8::ScriptCompiler::kConsumeCodeCache).ToLocal(&module)) {
            return {};
        }
        if (source.GetCachedData() && source.GetCachedData()->rejected) {
            std::unique_ptr<v8::ScriptCompiler::CachedData> fresh(
                v8::ScriptCompiler::CreateCodeCache(module->GetUnboundModuleScript()));
            Store(name, source_str, fresh.get());
        }
        return module;
    }

    v8::ScriptCompiler::Source source(src, origin);
    v8::Local<v8::Module> module;
    if (!v8::ScriptCompiler::CompileModule(
            isolate, &source, v8::ScriptCompiler::kEagerCompile).ToLocal(&module)) {
        return {};
    }
    std::unique_ptr<v8::ScriptCompiler::CachedData> fresh(
        v8::ScriptCompiler::CreateCodeCache(module->GetUnboundModuleScript()));
    Store(name, source_str, fresh.get());
    return module;
}

} // namespace next2d::codecache
