// platform/AssetIntegrity.h の単体テスト (V8 / Windows に依存しない)。
//   c++ -std=c++20 -I../src integrity_test.cpp -o integrity_test && ./integrity_test
//   cl /std:c++20 /EHsc /I..\src integrity_test.cpp && integrity_test.exe

#include "platform/AssetIntegrity.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failed = 0;
int g_checks = 0;

void Check(bool ok, const char* what)
{
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL: %s\n", what);
    }
}

std::string Hash(const std::string& text)
{
    return next2d::integrity::Sha256Hex(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

} // namespace

int main()
{
    using namespace next2d::integrity;

    // SHA-256 の既知の値 (FIPS 180-4 の例)
    Check(Hash("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 of empty");
    Check(Hash("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 of abc");
    Check(Hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
        == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "sha256 of two blocks");
    // 100 万個の "a" (ブロックをまたぐ追加と、長いデータ)
    {
        Sha256 hash;
        const std::string chunk(1000, 'a');
        for (int i = 0; i < 1000; ++i) {
            hash.Update(reinterpret_cast<const uint8_t*>(chunk.data()), chunk.size());
        }
        Check(hash.FinishHex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "sha256 of a million a");
    }
    // 55 / 56 / 64 バイト (パディングの境界)
    Check(Hash(std::string(55, 'x')).size() == 64 && Hash(std::string(55, 'x')) != Hash(std::string(56, 'x')), "padding boundary 55/56");
    Check(Hash(std::string(64, 'x')) != Hash(std::string(63, 'x')), "padding boundary 63/64");

    // 一覧が無ければ、何も確かめない
    SetManifest(nullptr);
    Check(!Enabled(), "disabled without a manifest");
    Check(Verify("assets/js/app.js", std::string("anything")), "everything passes when disabled");
    Check(!NeedsCheck("assets/js/app.js"), "no check needed when disabled");
    SetManifest("");
    Check(!Enabled(), "disabled with an empty manifest");

    // 一覧あり
    const std::string manifest =
        Hash("console.log(1);") + "  assets/js/app.js\n"
        + Hash("<html>") + "  index.html\r\n"
        + Hash("boot") + "  js/bootstrap.js\n"
        + "not a valid line\n";
    SetManifest(manifest.c_str());
    Check(Enabled(), "enabled with a manifest");

    Check(Verify("assets/js/app.js", std::string("console.log(1);")), "a listed file with the same content passes");
    Check(!Verify("assets/js/app.js", std::string("console.log(2);")), "a listed file with changed content fails");
    Check(Verify("index.html", std::string("<html>")), "a line ending with CR LF is read");
    Check(Verify("js/bootstrap.js", std::string("boot")), "the host script passes");
    Check(!Verify("js/bootstrap.js", std::string("boot ")), "the host script with changed content fails");

    // 一覧に無いファイル: スクリプト類は使わせない、それ以外は通す
    Check(!Verify("assets/js/extra.js", std::string("x")), "an unlisted script fails");
    Check(!Verify("data/added.JSON", std::string("{}")), "an unlisted json fails (extension is case-insensitive)");
    Check(!Verify("mod.wasm", std::string("")), "an unlisted wasm fails");
    Check(Verify("assets/bgm.mp3", std::string("sound")), "an unlisted media file passes");
    Check(Verify("noextension", std::string("")), "a file without an extension passes");

    // 少しずつ読む経路を使ってよいか
    Check(NeedsCheck("assets/js/app.js"), "a listed file needs the whole content");
    Check(NeedsCheck("assets/js/extra.js"), "an unlisted script needs the check");
    Check(!NeedsCheck("assets/bgm.mp3"), "media can be streamed");

    // 設定し直すと、前の一覧は残らない
    SetManifest((Hash("a") + "  only.js\n").c_str());
    Check(!Verify("index.html", std::string("<html>")), "the previous manifest is replaced");
    Check(Verify("only.js", std::string("a")), "the new manifest is used");
    SetManifest(nullptr);

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
