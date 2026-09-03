#pragma once

// This DTO intentionally has no Arduino dependency so route registration can
// be tested on the host. The concrete WebServer remains owned by the caller
// or by the legacy ConfigurationWeb implementation.
class WebServer;

using ConfigurationStorageBeginCallback = bool (*)();
using ConfigurationStorageEndCallback = bool (*)();

inline constexpr char CONFIGURATION_WEB_DEFAULT_PAGE_PATH[] = "/clock/";
inline constexpr char CONFIGURATION_WEB_DEFAULT_API_PREFIX[] =
    "/api/modules/clock";

struct ConfigurationWebRoutes {
  WebServer* webServer = nullptr;
  // The bundled pages target these stable canonical paths. A caller choosing
  // different values must supply page assets built for the same paths.
  const char* pagePath = CONFIGURATION_WEB_DEFAULT_PAGE_PATH;
  const char* apiPrefix = CONFIGURATION_WEB_DEFAULT_API_PREFIX;
  bool registerLegacyAliases = true;
  bool manageServerLifecycle = true;
  bool firmwareUpdatesEnabled = true;
  // The combined host supplies Meteo's radar screen separately.  Keep clock
  // radar routes/diagnostics optional so this web adapter does not require the
  // standalone CHMI service or display driver symbols when disabled.
  bool radarEnabled = true;
  // The combined host uses the shared bounded WebServer subclass as well.
  bool boundedPostSupport = false;
  // Supply both callbacks or neither. They bracket every runtime NVS write.
  ConfigurationStorageBeginCallback storageBegin = nullptr;
  ConfigurationStorageEndCallback storageEnd = nullptr;
};

using ConfigurationWebOptions = ConfigurationWebRoutes;
