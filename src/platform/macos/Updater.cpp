#include "core/Updater.h"

// Windows releases currently carry only an .exe installer. The macOS preview
// exposes manual updates in the UI and never downloads or launches that asset.
namespace rv {
Updater::Updater() = default;
Updater::~Updater() = default;
void Updater::start(bool) {}
void Updater::setAutomatic(bool) {}
void Updater::checkNow() {}
void Updater::download() {}
Updater::Status Updater::status() const { return {}; }
}
