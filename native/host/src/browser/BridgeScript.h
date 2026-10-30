#pragma once

#include <string>

namespace uxwv
{
/** Query function names; browser and renderer routers must agree on these. */
inline const char* BridgeQueryFunction()
{
	return "uxwvQuery";
}

inline const char* ExecQueryFunction()
{
	return "uxwvExec";
}

// Injected into every main frame after the message router installed its query
// function. Pages talk to the game through this object:
//   window.uxwv.call('name', args)  -> Promise resolved with the game's reply
//   window.uxwv.emit('name', data)  -> Promise resolved when the game got it
// Both send {name, payload}; the browser side forwards them to the plugin.
inline const std::string& BridgeBootstrapScript()
{
	static const std::string Script = R"JS(
(function () {
  if (window.uxwv) { return; }
  function encode(name, data) {
    try {
      return JSON.stringify({ name: String(name), payload: data === undefined ? null : data });
    } catch (e) {
      // Cyclic or otherwise unserializable payloads degrade to their text form.
      return JSON.stringify({ name: String(name), payload: String(data) });
    }
  }
  function send(name, data) {
    return new Promise(function (resolve, reject) {
      window.uxwvQuery({
        request: encode(name, data),
        persistent: false,
        onSuccess: function (response) {
          if (response === undefined || response === null || response === '') {
            resolve(null);
            return;
          }
          try { resolve(JSON.parse(response)); } catch (e) { resolve(response); }
        },
        onFailure: function (code, message) {
          reject(new Error(message || ('bridge failure ' + code)));
        }
      });
    });
  }
  window.uxwv = {
    call: send,
    emit: send
  };
})();
)JS";
	return Script;
}
} // namespace uxwv
