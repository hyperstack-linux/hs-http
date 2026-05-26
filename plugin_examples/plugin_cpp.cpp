#include "plugin_api.hpp"

// Globalny wskaźnik musi być utworzony przed loop()
Plugin *plugin = nullptr;

static void __attribute__((constructor(101))) init_plugin() {
  plugin = new Plugin("MyCppPlugin");
}

static void __attribute__((constructor(102))) loop() {
  if (!plugin)
    return;

  plugin->registerHandler("/", [](Request &req, Response &res) {
    res.sendText("Hello from instance-based plugin!");
  });

  plugin->registerHandler("/users", [](Request &req, Response &res) {
    res.sendJSON(R"({"users": ["alice", "bob", "charlie"]})");
  });

  plugin->registerHandler("/data", [](Request &req, Response &res) {
    Plugin::logDebug("Data endpoint: " + std::string(req.getFullPath()));
    res.sendText("Data from C++");
  });

  // Wildcard example: /files/* matches /files/foo, /files/bar/baz, etc.
  plugin->registerHandler("/files/*", [](Request &req, Response &res) {
    std::string body = "File path: ";
    body += req.getPath();
    res.sendText(body);
  });

  // Wildcard with prefix: /api/*/action matches /api/users/action, /api/items/action
  plugin->registerHandler("/api/*/action", [](Request &req, Response &res) {
    res.sendJSON(R"({"status": "action performed"})");
  });

  plugin->registerHandler(
      "/echo",
      [](Request &req, Response &res) {
        const char *name = req.getBodyParam("email");
        const char *header =
            req.getHeader("Authorization"); // przykładowe użycie
        if (name) {
          res.sendText(
              std::string("Echo: ") + std::string(name) +
              "\nAuthorization header: " + (header ? header : "(none)"));
        } else {
          res.sendText("No name parameter provided.");
        }
      },
      "POST");

  plugin->registerHandler("/query", [](Request &req, Response &res) {
    std::string response = "Query params:\n";

    for (int i = 0; i < req.getQueryParamCount(); i++) {
      response += std::string(req.getQueryParamKey(i)) + " = " +
                  std::string(req.getQueryParamValue(i)) + "\n";
    }

    const char *name = req.getQueryParam("name");
    if (name) {
      response += "\nHello, " + std::string(name) + "!";
    }

    res.sendText(response);
  });

  plugin->registerHandler("/sse", [](Request &req, Response &res) {
    res.startSSE();

    for (int i = 0; i < 10; i++) {
      res.sendSSE("Wiadomosc " + std::to_string(i), "message");
    }

    res.closeSSE();
  });

  // plugin->setInitCallback([]() {
  //     Plugin::logInfo("Plugin initialized via callback!");
  // });

  // plugin->setCleanupCallback([]() {
  //     Plugin::logInfo("Plugin cleanup via callback!");
  // });
}
