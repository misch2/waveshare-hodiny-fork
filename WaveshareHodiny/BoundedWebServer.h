#pragma once

#include <WebServer.h>
#include <esp_heap_caps.h>

#include <cstring>

// WebServer's default form parser can allocate according to attacker supplied
// field lengths.  Keep the bounded parser reusable for both the standalone
// server and the combined host's caller-owned WebServer instance.
class BoundedWebServer : public WebServer {
 public:
  using WebServer::WebServer;
  using WebServer::arg;
  using WebServer::hasArg;

  void beginBoundedPostSupport() {
    if (postBody_ == nullptr) {
      postBody_ = static_cast<char *>(heap_caps_malloc(
          MAX_POST_BODY_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
  }

  void captureRawPost(HTTPRaw &raw) {
    if (raw.status == RAW_START) {
      // Avoid waiting for the default raw-body timeout after a short final
      // block, while still bounding slow POST requests.
      client().setTimeout(100);
      rawPostActive_ = true;
      postBodyLength_ = 0;
      postBodyReady_ = false;
      postBodyTooLarge_ = clientContentLength() > MAX_POST_BODY_BYTES;
      postBodyMalformed_ = false;
      if (postBody_ != nullptr) postBody_[0] = '\0';
      return;
    }
    if (raw.status == RAW_WRITE) {
      if (postBodyTooLarge_ || postBody_ == nullptr) return;
      if (raw.currentSize > MAX_POST_BODY_BYTES - postBodyLength_) {
        postBodyTooLarge_ = true;
        return;
      }
      memcpy(postBody_ + postBodyLength_, raw.buf, raw.currentSize);
      postBodyLength_ += raw.currentSize;
      return;
    }
    if (raw.status == RAW_END) {
      if (!postBodyTooLarge_ && postBody_ != nullptr) {
        postBody_[postBodyLength_] = '\0';
        // JSON/plain requests (the combined export/import endpoint) are
        // bounded by the same raw buffer but do not contain form fields.
        postBodyMalformed_ = isFormEncoded() && !validRawBodyShape();
        postBodyReady_ = !postBodyMalformed_;
      }
      return;
    }
    endBoundedPostRequest();
  }

  bool postBodyAccepted() const {
    return rawPostActive_ && postBodyReady_ && !postBodyTooLarge_;
  }

  bool postBodyTooLarge() const { return postBodyTooLarge_; }
  bool postBodyMalformed() const { return postBodyMalformed_; }

  // The raw parser is request-scoped.  Clear it after the matching handler so
  // subsequent GETs and non-bounded handlers use WebServer's normal parser.
  void endBoundedPostRequest() {
    rawPostActive_ = false;
    postBodyReady_ = false;
    postBodyTooLarge_ = false;
    postBodyMalformed_ = false;
    postBodyLength_ = 0;
    if (postBody_ != nullptr) postBody_[0] = '\0';
  }

  String arg(const String &name) {
    if (!rawPostActive_) return WebServer::arg(name);
    if (name == "plain" && !isFormEncoded()) {
      return postBodyAccepted() ? String(postBody_, postBodyLength_) : String();
    }
    size_t valueStart = 0;
    size_t valueLength = 0;
    if (!findRawArgument(name, valueStart, valueLength)) return String();
    return WebServer::urlDecode(String(postBody_ + valueStart, valueLength));
  }

  bool hasArg(const String &name) {
    if (!rawPostActive_) return WebServer::hasArg(name);
    if (name == "plain" && !isFormEncoded()) return postBodyAccepted();
    size_t valueStart = 0;
    size_t valueLength = 0;
    return findRawArgument(name, valueStart, valueLength);
  }

 private:
  static constexpr size_t MAX_POST_BODY_BYTES = 16 * 1024;
  static constexpr size_t MAX_POST_KEY_BYTES = 64;
  static constexpr size_t MAX_POST_VALUE_BYTES = 1024;

  bool isFormEncoded() {
    const String contentType = header("Content-Type");
    return contentType.isEmpty() ||
           contentType.startsWith("application/x-www-form-urlencoded");
  }

  bool validRawBodyShape() const {
    size_t fieldStart = 0;
    while (fieldStart < postBodyLength_) {
      size_t fieldEnd = fieldStart;
      while (fieldEnd < postBodyLength_ && postBody_[fieldEnd] != '&')
        ++fieldEnd;
      size_t equalsAt = fieldStart;
      while (equalsAt < fieldEnd && postBody_[equalsAt] != '=') ++equalsAt;
      if (equalsAt == fieldEnd || equalsAt - fieldStart > MAX_POST_KEY_BYTES ||
          fieldEnd - equalsAt - 1 > MAX_POST_VALUE_BYTES) {
        return false;
      }
      fieldStart = fieldEnd + 1;
    }
    return true;
  }

  bool findRawArgument(const String &name, size_t &valueStart,
                       size_t &valueLength) const {
    if (!postBodyAccepted()) return false;
    size_t fieldStart = 0;
    while (fieldStart <= postBodyLength_) {
      size_t fieldEnd = fieldStart;
      while (fieldEnd < postBodyLength_ && postBody_[fieldEnd] != '&')
        ++fieldEnd;
      size_t equalsAt = fieldStart;
      while (equalsAt < fieldEnd && postBody_[equalsAt] != '=') ++equalsAt;
      if (equalsAt < fieldEnd) {
        const String encodedName(postBody_ + fieldStart, equalsAt - fieldStart);
        if (WebServer::urlDecode(encodedName) == name) {
          valueStart = equalsAt + 1;
          valueLength = fieldEnd - valueStart;
          return true;
        }
      }
      if (fieldEnd == postBodyLength_) break;
      fieldStart = fieldEnd + 1;
    }
    return false;
  }

  char *postBody_ = nullptr;
  size_t postBodyLength_ = 0;
  bool rawPostActive_ = false;
  bool postBodyReady_ = false;
  bool postBodyTooLarge_ = false;
  bool postBodyMalformed_ = false;
};

// Shared route adapter: both the standalone application and the combined host
// register bounded URL-encoded POST handlers through this one implementation.
class BoundedPostRequestHandler final : public RequestHandler {
 public:
  BoundedPostRequestHandler(BoundedWebServer &server, const char *uri,
                            WebServer::THandlerFunction handler,
                            WebServer::THandlerFunction rejected = nullptr)
      : server_(server), uri_(uri), handler_(handler), rejected_(rejected) {}

  bool canHandle(WebServer &, HTTPMethod method, const String &uri) override {
    return method == HTTP_POST && uri == uri_;
  }

  bool canRaw(WebServer &, const String &uri) override { return uri == uri_; }

  bool handle(WebServer &, HTTPMethod, const String &) override {
    if (server_.postBodyAccepted()) {
      handler_();
    } else if (rejected_ != nullptr) {
      rejected_();
    }
    server_.endBoundedPostRequest();
    return true;
  }

  void raw(WebServer &, const String &, HTTPRaw &raw) override {
    server_.captureRawPost(raw);
  }

 private:
  BoundedWebServer &server_;
  String uri_;
  WebServer::THandlerFunction handler_;
  WebServer::THandlerFunction rejected_;
};

inline void registerBoundedPost(
    WebServer &server, BoundedWebServer *boundedServer, const String &uri,
    WebServer::THandlerFunction handler,
    WebServer::THandlerFunction rejected = nullptr) {
  if (boundedServer != nullptr) {
    server.addHandler(new BoundedPostRequestHandler(
        *boundedServer, uri.c_str(), handler, rejected));
  } else {
    server.on(uri, HTTP_POST, handler);
  }
}
