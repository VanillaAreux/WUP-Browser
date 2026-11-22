# WUP Browser

A lightweight, touch-first HTTPS browser for the Wii U GamePad, built with `libwup`.

## Build

From the `libwup` directory in the SDK:

```sh
python3 tools/wupbuild.py /path/to/wup-browser
```

The `.wup` is written to `libwup/build/apps/wup_browser.wup`.

## Use

Connect to Wi-Fi from the Homebrew Menu first so it can set the GamePad's clock. Open WUP Browser, tap the address field, enter a site, and press Go. Links appear as buttons beneath the page text. Back and reload are in the top bar.

## Limitations

This is a lightweight text-first browser, not a Chromium/WebKit port. It downloads HTTPS pages (up to 96 KiB), extracts basic HTML text and links, and does not execute JavaScript, apply CSS layouts, render images, or play media. Sites that require JavaScript may not work. HTTP addresses are upgraded to HTTPS. TLS certificate verification is enabled.
