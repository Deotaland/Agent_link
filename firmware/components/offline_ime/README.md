# Offline touch input engine

Reusable nine-key/full-keyboard pinyin, English and symbol input, independent of
board, display, touch driver and transport. Initialize the
[Paper resources](../paper_resources/README.md) before using Chinese candidates.
`paper4_ime.h` is the desktop's compatibility alias for this component.

The engine scans the mapped dictionary, ranks matches, and retains all matching
characters beyond the first candidate page. A candidate contains a pointer view
of strings in flash, its frequency and the consumed composition length. It does
not copy the full dictionary. Draft length, UTF-8 backspace and independent
pending nine-key/full-keyboard composition behavior are preserved.

No network recognition, handwriting, sentence learning or message sending occurs
in this component. The app owns the keyboard UI and explicit send confirmation.
