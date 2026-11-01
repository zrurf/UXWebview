# Script bridge

Pages talk to the game through a small injected API. Every call crosses the
process boundary (renderer -> host -> plugin) and is answered by game code.

## Page side

```js
// Resolves with the game's JSON reply, rejects with a bridge error.
window.uxwv.call('getPlayerState', { id: 42 }).then(function (reply) {
  console.log(reply.level);
});

// Notification: still answered by the game, so the page can confirm delivery.
window.uxwv.emit('opened', { tab: 'inventory' });
```

Both functions cross the bridge identically and both need a
`RespondToScriptMessage` call. `emit` is meant for notifications and `call` for
requests, but neither is fire-and-forget: an unanswered call rejects after 30
seconds. Payloads must survive `JSON.stringify`; the reply text is parsed with
`JSON.parse` and falls back to the raw string. A successful reply with an empty
payload resolves with `null`.

## Game side

```cpp
void UMyWidget::OnWebviewReady()
{
    Webview->OnScriptMessage.AddDynamic(this, &UMyWidget::HandleScriptMessage);
}

void UMyWidget::HandleScriptMessage(const FString& Name, const FString& PayloadJson, int32 RequestId)
{
    if (Name == TEXT("getPlayerState"))
    {
        Webview->RespondToScriptMessage(RequestId, true, TEXT("{\"level\":12}"));
    }
    else
    {
        Webview->RespondToScriptMessage(RequestId, false, TEXT("\"unknown\""));
    }
}
```

- `RespondToScriptMessage` must be called exactly once per request.
- A request left unanswered fails with `timeout` after 30 seconds.
- Both CEF and WebView2 use the same message shape and origin policy.
- Game → page: `EmitToWeb(Name, PayloadJson)` dispatches a `uxwv` CustomEvent
  whose `detail` is `{ name, payload }`.

## Reserved name: `request-keyboard`

A composition overlay renders transparent pages but cannot deliver typed input.
A page that needs the keyboard asks for it instead of guessing the hosting mode:

```js
document.querySelector('#search').addEventListener('focusin', function () {
  window.uxwv.call('request-keyboard', { field: 'search' });
});
```

This name never reaches `OnScriptMessage`. The plugin broadcasts
`OnKeyboardRequest(PayloadJson)` and resolves the promise with `{}` right after
the delegates run, whether or not a handler is bound. Game code decides how to
react; `EnsureKeyboardInput()` is the one-line answer, switching the view back
to a regular controller (a no-op when it already has one). The switch reloads the
page, so scroll position, form values and focus are restored afterwards.

## Origin policy

Local schemes (`file:`, `data:`, `about:`, `blob:`) are always allowed. Every
other origin must be listed in **Project Settings > Plugins > UXWebview >
Bridge Allowed Origins**, otherwise the page receives
`BridgeDenied: origin not allowed`. An empty list therefore blocks all remote
pages while local test pages keep working.

## Failure modes

| Failure | Page sees |
| --- | --- |
| Origin not allowlisted | `Error: BridgeDenied: origin not allowed` |
| Malformed request | `Error: BridgeBadRequest: malformed message` |
| Game never replies | `Error: timeout` |
| Host or view destroyed first | `Error: the view is not available` |

Errors reject the promise; they never crash the page or the game.
