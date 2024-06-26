#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netdb.h>
#include <thread>
#include <chrono>
#include <string>
#include <fstream>
#include <csignal>

#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/write_resume_data.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/magnet_uri.hpp>

#include <unistd.h>

namespace {

using clk = std::chrono::steady_clock;

// return the name of a torrent status enum
char const* state(lt::torrent_status::state_t s)
{
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcovered-switch-default"
#endif
  switch(s) {
    case lt::torrent_status::checking_files: return "checking";
    case lt::torrent_status::downloading_metadata: return "dl metadata";
    case lt::torrent_status::downloading: return "downloading";
    case lt::torrent_status::finished: return "finished";
    case lt::torrent_status::seeding: return "seeding";
    case lt::torrent_status::checking_resume_data: return "checking resume";
    default: return "<>";
  }
#ifdef __clang__
#pragma clang diagnostic pop
#endif
}

char * getSession(const char * rootdir){
  char * str = (char *)malloc(sizeof(char) * 25 + sizeof(rootdir));
  sprintf(str,"%s/unpkg/.session", rootdir);
  return str;
}

char * getResumeFile(const char * rootdir){
  char * str = (char *)malloc(sizeof(char) * 25 + sizeof(rootdir));
  sprintf(str,"%s/unpkg/.resume_file", rootdir);
  return str;
}

char * getSavePath(const char * rootdir){
  char * str = (char *)malloc(sizeof(char) * 25 + sizeof(rootdir));
  sprintf(str,"%s/unpkg/cache/torrentdir", rootdir);
  return str;
}

std::vector<char> load_file(char const* filename)
{
  std::ifstream ifs(filename, std::ios_base::binary);
  ifs.unsetf(std::ios_base::skipws);
  return {std::istream_iterator<char>(ifs), std::istream_iterator<char>()};
}

// set when we're exiting
std::atomic<bool> shut_down{false};

void sighandler(int) { shut_down = true; }

} // anonymous namespace

int install(const char * magnet_link, const char * rootdir) try
{
  if (magnet_link == NULL || rootdir == NULL) {
    std::cerr << "usage: unpgk-torrent <magnet-url> <unpkg root dir>" << std::endl;
    return 1;
  }
  // load session parameters
  auto session_params = load_file(getSession(rootdir));
  lt::session_params params = session_params.empty()
    ? lt::session_params() : lt::read_session_params(session_params);
  params.settings.set_int(lt::settings_pack::alert_mask
    , lt::alert_category::error
    | lt::alert_category::storage
    | lt::alert_category::status);

  lt::session ses(params);
  clk::time_point last_save_resume = clk::now();

  // load resume data from disk and pass it in as we add the magnet link
  auto buf = load_file(getResumeFile(rootdir));
  lt::add_torrent_params magnet = lt::parse_magnet_uri(magnet_link);
  if (buf.size()) {
    lt::add_torrent_params atp = lt::read_resume_data(buf);
    if (atp.info_hashes == magnet.info_hashes) magnet = std::move(atp);
  }

  magnet.save_path = getSavePath(rootdir); // save in current dir
  ses.async_add_torrent(std::move(magnet));

  // this is the handle we'll set once we get the notification of it being
  // added
  lt::torrent_handle h;

  std::signal(SIGINT, &sighandler);

  // set when we're exiting
  bool done = false;
  for (;;) {
    std::vector<lt::alert*> alerts;
    ses.pop_alerts(&alerts);

    if (shut_down) {
      shut_down = false;
      auto const handles = ses.get_torrents();
      if (handles.size() == 1) {
        handles[0].save_resume_data(lt::torrent_handle::only_if_modified
          | lt::torrent_handle::save_info_dict);
        done = true;
      }
    }

    for (lt::alert const* a : alerts) {
      if (auto at = lt::alert_cast<lt::add_torrent_alert>(a)) {
        h = at->handle;
      }
      // if we receive the finished alert or an error, we're done
      if (lt::alert_cast<lt::torrent_finished_alert>(a)) {
        h.save_resume_data(lt::torrent_handle::only_if_modified
          | lt::torrent_handle::save_info_dict);
        done = true;
      }
      if (lt::alert_cast<lt::torrent_error_alert>(a)) {
        std::cout << a->message() << std::endl;
        done = true;
        h.save_resume_data(lt::torrent_handle::only_if_modified
          | lt::torrent_handle::save_info_dict);
      }

      // when resume data is ready, save it
      if (auto rd = lt::alert_cast<lt::save_resume_data_alert>(a)) {
        std::ofstream of(getResumeFile(rootdir), std::ios_base::binary);
        of.unsetf(std::ios_base::skipws);
        auto const b = write_resume_data_buf(rd->params);
        of.write(b.data(), int(b.size()));
        if (done) goto done;
      }

      if (lt::alert_cast<lt::save_resume_data_failed_alert>(a)) {
        if (done) goto done;
      }

      if (auto st = lt::alert_cast<lt::state_update_alert>(a)) {
        if (st->status.empty()) continue;

        // we only have a single torrent, so we know which one
        // the status is for
        lt::torrent_status const& s = st->status[0];
        std::cout << '\r' << state(s.state) << ' '
          << (s.download_payload_rate / 1000) << " kB/s "
          << (s.total_done / 1000) << " kB ("
          << (s.progress_ppm / 10000) << "%) downloaded ("
          << s.num_peers << " peers)\x1b[K";
        std::cout.flush();
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // ask the session to post a state_update_alert, to update our
    // state output for the torrent
    ses.post_torrent_updates();

    // save resume data once every 30 seconds
    if (clk::now() - last_save_resume > std::chrono::seconds(30)) {
      h.save_resume_data(lt::torrent_handle::only_if_modified
        | lt::torrent_handle::save_info_dict);
      last_save_resume = clk::now();
    }
  }

done:
  std::cout << "\nsaving session state" << std::endl;
  {
    std::ofstream of(getSession(rootdir), std::ios_base::binary);
    of.unsetf(std::ios_base::skipws);
    auto const b = write_session_params_buf(ses.session_state()
      , lt::save_state_flags_t::all());
    of.write(b.data(), int(b.size()));
  }

  std::cout << "\ndone" << std::endl;
  return EXIT_SUCCESS;
}
catch (std::exception& e)
{
  std::cerr << "Error: " << e.what() << std::endl;
  return EXIT_FAILURE;
}

int createMagnet(const char * domain, const char * root){
  return EXIT_SUCCESS;
}

int main(int argc, char* argv[]) {
    int opt;
    const char * link, * directory;
    bool useMagnet = false;

    // Options descriptor
    while ((opt = getopt(argc, argv, "m:c:")) != -1) {
        switch (opt) {
            case 'm':
                link = optarg;
                useMagnet = true;
                break;
            case 'c':
                link = optarg;
                useMagnet = false;
                break;
            case '?': // Something wrong with the options
                if (optopt == 'm' || optopt == 'c')
                    std::cerr << "Option -" << static_cast<char>(optopt) << " requires an argument." << std::endl;
                else
                    std::cerr << "Unknown option: -" << static_cast<char>(optopt) << std::endl;
                return 1;
            default:
                return 1;
        }
    }

    directory = argv[optind];

    if (useMagnet) {
        install(link, directory);
    } else if (!useMagnet) {
        return createMagnet(link, directory);
    } else {
        std::cerr << "No valid resource provided." << std::endl;
        return 1;
    }
    return 0;
}
