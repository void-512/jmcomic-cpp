#include "jmcomic/tui.hpp"
#include "jmcomic/storage.hpp"
#include <iostream>
#include <string_view>
int main(int argc, char** argv) {
  if (argc==2 && std::string_view(argv[1])=="--help") {
    std::cout << "JMComic downloader\nUsage: jmcomic [--help]\nSearch, account login, favorites, chapter selection, downloads and settings are in the TUI.\nTab changes focus; arrows navigate; Space selects; Enter activates; Ctrl-Q quits.\nConfig: jmcomic.json and jmcomic-session.json beside the executable.\n";return 0;
  }
  if (argc!=1) {std::cerr<<"Usage: jmcomic [--help]\n";return 2;}
  try {
    const auto directory=jm::executable_directory(argv[0]);
    return jm::run_tui(directory,jm::load_settings(directory));
  } catch (const jm::Json::exception&) {
    std::cerr<<"Cannot read local settings/session: invalid JSON or unexpected fields. Repair the files beside the executable.\n";
  } catch (const std::exception& e) { std::cerr<<"JMComic: "<<e.what()<<'\n'; }
  return 1;
}
