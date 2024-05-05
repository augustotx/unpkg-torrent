#include <libtorrent/session.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/bdecode.hpp>
#include <fstream>
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>

void seed_torrent(const std::string& torrent_file, const std::string& save_path) {
    // Create a session
    lt::session sess;

    // Session settings (optional)
    lt::settings_pack sp;
    sp.set_int(lt::settings_pack::alert_mask, lt::alert_category::error | lt::alert_category::storage);
    sess.apply_settings(sp);

    // Load the torrent file into buffer
    std::ifstream input(torrent_file, std::ios_base::binary);
    std::vector<char> buffer(std::istreambuf_iterator<char>(input), {});

    // Decode the torrent file
    lt::error_code ec;
    lt::bdecode_node torrent_bdecode;
    lt::bdecode(&buffer[0], &buffer[0] + buffer.size(), torrent_bdecode, ec);
    if (ec) {
        std::cerr << "Error decoding torrent file: " << ec.message() << std::endl;
        return;
    }

    // Create a torrent_info object from the decoded node
    lt::torrent_info ti(torrent_bdecode, ec);
    if (ec) {
        std::cerr << "Error constructing torrent_info: " << ec.message() << std::endl;
        return;
    }

    // Add the torrent to the session
    lt::add_torrent_params params;
    params.ti = std::make_shared<lt::torrent_info>(ti);
    params.save_path = save_path; // Set the save path
    sess.add_torrent(params);

    std::cout << "Seeding torrent: " << ti.name() << std::endl;

    // Run the session
    for (;;) {
        std::vector<lt::alert*> alerts;
        sess.pop_alerts(&alerts);

        for (lt::alert* alert : alerts) {
            // Print status messages to console
            std::cout << alert->message() << std::endl;

            // Check for torrent finished and other alerts
            if (auto at = lt::alert_cast<lt::torrent_finished_alert>(alert)) {
                std::cout << "Torrent finished seeding: " << at->handle.torrent_file()->name() << std::endl;
                return; // or sess.pause(); to stop seeding when done
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <torrent_file> <save_path>" << std::endl;
        return 1;
    }

    std::string torrent_file = argv[1];
    std::string save_path = argv[2];

    seed_torrent(torrent_file, save_path);

    return 0;
}
