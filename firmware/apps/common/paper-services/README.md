# Shared Paper services

Reusable ES8389 playback/capture and SD WAV music services for the independent
Paper V2 six-color and Paper 4IN0 applications. These files contain no menu,
key mapping, platform product identity or application registration. Each app
selects `shared_app: "paper-services"` in its manifest and supplies its board's
`config.h` and Paper hardware APIs.

`paper_protocol.h` retains the existing six-color app's validated JSON tool
helpers, WAV parser and date helpers. The 4IN0 music service uses the WAV/path
validation without imposing that app's chat reply protocol.
