#if CARDPUTER_ADV_UI
#include "companion_radio/ui-cardputer/screen/SplashScreen.h"
#include "companion_radio/ui-cardputer/screen/MainScreen.h"
#include "companion_radio/ui-cardputer/screen/NewMessageScreen.h"
#include "companion_radio/ui-cardputer/screen/SettingsScreen.h"
#include "companion_radio/ui-cardputer/screen/ToolsScreen.h"
#include "companion_radio/ui-cardputer/screen/main/FirstPage.h"
#include "companion_radio/ui-cardputer/screen/main/ContactsPage.h"
#include "companion_radio/ui-cardputer/screen/main/ChannelsPage.h"
#include "companion_radio/ui-cardputer/screen/main/ChatPage.h"
#include "companion_radio/ui-cardputer/screen/main/RecentAdvertsPage.h"
#include "companion_radio/ui-cardputer/screen/main/StatsPage.h"
#include "companion_radio/ui-cardputer/screen/main/GpsPage.h"
#include "hw/CardputerLayout.h"

// Bind the heap admission model to the actual target ABI. Strings and driver
// allocations have separate additive allowances in check_firmware_ram.py.
static_assert(sizeof(SplashScreen) + sizeof(MainScreen) + sizeof(NewMessageScreen)
    + sizeof(SettingsScreen) + sizeof(ToolsScreen) + sizeof(FirstPage)
    + sizeof(ContactsPage) + sizeof(ChannelsPage) + sizeof(ChatPage)
    + sizeof(RecentAdvertsPage) + sizeof(StatsPage) + sizeof(GpsPage) <= 8192,
    "Cardputer screen objects exceed the heap allowance");
static_assert(sizeof(Keyboard::Layout::LayoutLayer) == 225,
    "Cardputer keyboard layer size changed; update the heap allowance");
#endif
