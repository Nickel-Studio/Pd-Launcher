#include <archive.h>
#include <archive_entry.h>
#include <bzlib.h>
#include <curl/curl.h>
#include <gumbo.h>
#include <lzma.h>
#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>
#include <yaml.h>
#include <zlib.h>
#include <zstd.h>

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

int main() {
    try {
        const auto* curl = curl_version_info(CURLVERSION_NOW);
        if (!curl || curl->version_num != 0x081700 || !(curl->features & CURL_VERSION_SSL))
            throw std::runtime_error("Pinned curl/TLS unavailable");
#ifdef _WIN32
        if (!curl->ssl_version || std::strstr(curl->ssl_version, "Schannel") == nullptr)
            throw std::runtime_error("Expected Windows Schannel backend");
#endif
        if (archive_version_number() != 3008009 || std::strcmp(zlibVersion(), "1.3.1") != 0
            || lzma_version_number() != 50080012U || ZSTD_versionNumber() != 10507
            || std::strncmp(BZ2_bzlibVersion(), "1.0.8", 5) != 0
            || std::strcmp(yaml_get_version_string(), "0.2.5") != 0)
            throw std::runtime_error("Installed compression/parser versions differ from lock");
        auto* html = gumbo_parse("<h1>Nickel</h1>");
        if (!html || !html->root) throw std::runtime_error("Gumbo failed");
        gumbo_destroy_output(&kGumboDefaultOptions, html);
        const auto toml = toml::parse("native = true");
        if (!toml["native"].value_or(false)) throw std::runtime_error("TOML failed");
        yaml_parser_t yaml{};
        if (!yaml_parser_initialize(&yaml)) throw std::runtime_error("YAML init failed");
        const unsigned char input[] = "native: true\n";
        yaml_parser_set_input_string(&yaml, input, sizeof(input) - 1);
        yaml_document_t document{};
        const auto ok = yaml_parser_load(&yaml, &document);
        yaml_parser_delete(&yaml);
        if (!ok) throw std::runtime_error("YAML parse failed");
        yaml_document_delete(&document);
        std::array<unsigned char, 512> bytes{};
        std::size_t used = 0;
        auto* writer = archive_write_new();
        archive_write_set_format_zip(writer);
        if (archive_write_open_memory(writer, bytes.data(), bytes.size(), &used) != ARCHIVE_OK)
            throw std::runtime_error("Archive memory writer failed");
        auto* entry = archive_entry_new();
        archive_entry_set_pathname(entry, "native.txt");
        archive_entry_set_size(entry, 2);
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0600);
        const bool written = archive_write_header(writer, entry) == ARCHIVE_OK
            && archive_write_data(writer, "Ni", 2) == 2;
        archive_entry_free(entry);
        archive_write_close(writer);
        archive_write_free(writer);
        if (!written || !used) throw std::runtime_error("Static archive ZIP failed");
        nlohmann::json result{{"curl", curl->version}, {"tls", curl->ssl_version},
            {"archive", archive_version_string()}, {"zlib", zlibVersion()},
            {"lzma", lzma_version_string()}, {"zstd", ZSTD_versionString()},
            {"bzip2", BZ2_bzlibVersion()}, {"yaml", yaml_get_version_string()},
            {"gumbo", "0.10.1"}, {"tomlplusplus", "3.4.0"}, {"zipBytes", used}};
        std::cout << result.dump(2) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
