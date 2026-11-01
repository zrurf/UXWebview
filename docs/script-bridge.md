# Script bridge

Pages talk to the game through a small injected API. Every call crosses the
process boundary (renderer -> host -> plugin) and is answered by game code.

## Page side

```js
// Resolves with the game's JSON reply, rejects with a bridge error.
window.uxwv.call('getPlayerState', { id: 42 }).then(function (reply) {
  console.log(reply.level);
});

// Fire-and-forget: resolves once the game received the event.
window.uxwv.emit('opened', { tab: 'inventory' });
```

Both functions return a Promise. Payloads must survive `JSON.stringify`; the
reply is parsed with `JSON.parse` and falls back to the raw string. A missing
reply resolves with `null`.

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
- A query left unanswered fails with `timeout` after 30 seconds.
- Game → page: `EmitToWeb(Name, PayloadJson)` dispatches a `uxwv` CustomEvent
  whose `detail` is `{ name, payload }`.

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
