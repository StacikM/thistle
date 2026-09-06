# In-app purchases

Poll-based, same shape as [`Http`](platform-and-networking.md): kick a request off, poll for the result each frame. Backed by StoreKit on iOS/macOS. Nowhere else — see the platform section at the bottom before you plan a cross-platform monetization scheme around this.

```cpp
iap_fetch_products({"com.yourgame.remove_ads", "com.yourgame.coins_100"});

app.update([&](Frame f) {
    if (iap_products_ready()) {
        for (auto& p : iap_products())
            f.text(p.title + " — " + p.price_string, pos, {});   // p.price_string is already localized
    }

    if (f.button("Buy", area)) iap_purchase("com.yourgame.remove_ads");

    IAPEvent e;
    while (iap_poll_event(e)) {
        if (e.kind == IAPEventKind::Purchased) {
            unlock_content(e.product_id);          // do this durably (save it) BEFORE finishing
            iap_finish_transaction(e.transaction_id);
        } else if (e.kind == IAPEventKind::Restored) {
            unlock_content(e.product_id);
            iap_finish_transaction(e.transaction_id);
        } else if (e.kind == IAPEventKind::Failed) {
            show_error(e.error_message);
        }
    }
});
```

## The rule that actually matters: finish transactions only after delivery is durable

`iap_finish_transaction()` tells StoreKit "I'm done, stop reminding me about this purchase." If you finish a transaction *before* you've actually saved that the user owns whatever they paid for, and the app crashes or the device dies in between, **you've taken their money and given them nothing, with no way for StoreKit to tell you again** — you already said you were done. Order matters: write to `save::` (or wherever your content-ownership state lives) *first*, then call `iap_finish_transaction()`. `Failed` transactions are finished for you automatically (there's nothing to deliver), but `Purchased`/`Restored`/`Deferred` are left for you to finish on purpose, on your own schedule, once delivery is real.

If you never finish a transaction, StoreKit keeps handing it back to you as a fresh `IAPEvent` on every subsequent launch — this is a *feature*, not a bug (it's how someone who force-quit mid-purchase eventually gets their content), but it means a bug in your own finish-logic looks like "the same purchase keeps firing forever," which is exactly what it is.

## `you can't purchase an id you haven't fetched`

`iap_purchase("some.id")` only works if `"some.id"` was in a *completed* `iap_fetch_products()` call and Apple actually returned a matching product (unconfigured/misspelled ids are silently absent from the results, not an error). Call it before fetching, or for an id Apple didn't recognize, and it comes back as an immediate `IAPEventKind::Failed` with an explanatory `error_message` — not a silent no-op, because that class of bug (wrong product id, forgot to fetch first) is common enough to deserve a loud signal instead of "nothing happened, good luck debugging that."

## What's actually been verified, and what hasn't

The request lifecycle is real and tested: `iap_can_make_payments()` calls the real `SKPaymentQueue.canMakePayments()`, and `iap_fetch_products()` fires a genuine `SKProductsRequest` at Apple's servers and gets a real response back — confirmed by actually running it (see `examples/smoketest.cpp`), not just compiling it. What **hasn't** been exercised: an actual purchase completing, because that needs product identifiers configured in App Store Connect (which requires an app record + the Paid Applications Agreement — neither exists yet for this project) or a local `.storekit` configuration file wired into an Xcode scheme for StoreKit's offline testing mode. Either of those would let you drive a real `Purchased`/`Failed`/`Restored` event through this exact code path; until one exists, the purchase/finish/restore functions are implemented against the documented StoreKit API and match its contract, but haven't produced an observed `Purchased` event. Don't take "the code is here" as "a purchase has been proven to work" until one of those is set up.

## StoreKit 1, not StoreKit 2, and why

StoreKit 2 (`Product.purchase()`, Swift async/await) is what Apple wants new code to use, but it's Swift-only with no C or Objective-C surface at all — there is no way to call it from C++. StoreKit 1 (`SKPaymentQueue`/`SKProduct`/`SKPaymentTransaction`, the classic Objective-C API this is built on) was deprecated in iOS 18 / macOS 15, but **deprecated is not removed** — it's still fully functional, and will be for a long time (too many shipped apps depend on it for Apple to pull it). If you eventually want StoreKit 2 specifically, that's a small Swift shim exporting `@_cdecl` functions that this engine's C++ calls into — real, separable work, not a rewrite of what's here.

## Platform support

iOS and macOS only, via StoreKit. Everywhere else — Windows, Linux, and Android — `iap_can_make_payments()` returns `false`, `iap_fetch_products()`/`iap_purchase()` are no-ops (the latter logs a warning), and `iap_products_ready()` is `true` with an empty list. Your game's buy-button code doesn't need an `#ifdef`; check `iap_can_make_payments()` before showing it and the platform difference disappears on its own.

Android specifically: there's no bridge here, and there's nothing *to* bridge to yet — there's no working Android build target in this engine at all (see [building.md](building.md)), just an unexercised link-library stub in `CMakeLists.txt`. Adding Google Play Billing support is real, separate work that only makes sense after Android actually builds and runs something.
