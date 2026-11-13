// A TextMate grammar for GStreamer pipeline descriptions, so ```gst fences are highlighted like
// the rest of the code (Expressive Code loads it through `shiki.langs` in astro.config.mjs).
// spec: docs/19-website-and-publishing.md#authoring-rules
//
// It reads gst-launch syntax and the docs' shorthand for it:
//   v4l2src device=/dev/video0 ! image/jpeg,width=1280,framerate=30/1 ! jpegdec      (gst-launch)
//   appsrc(is-live, format=time) ! rtp{h264,vp8,opus}pay ! webrtcbin.sink_%u         (shorthand)
//   [per track]  (one producer)  capture ─ encode ──► FrameHub                        (annotations)
// Scopes are ordinary TextMate ones, so the site's themes colour them: elements as tags, `!` and
// arrows as operators, properties as attributes, caps as types, notes as comments.

const value = {
  patterns: [
    { name: "string.quoted.double.gst", match: '"(?:[^"\\\\]|\\\\.)*"' },
    { name: "string.quoted.single.gst", match: "'[^']*'" },
    { name: "constant.numeric.gst", match: "-?\\b\\d+(?:[./]\\d+)?\\b(?![\\w/-])" },
    { name: "constant.language.gst", match: "\\b(?:true|false|TRUE|FALSE)\\b" },
    { name: "string.unquoted.gst", match: "[^\\s!,()\\[\\]]+" },
  ],
};

export const gstGrammar = {
  name: "gst",
  displayName: "GStreamer pipeline",
  scopeName: "source.gst",
  aliases: ["gstreamer", "gst-launch"],
  patterns: [
    { include: "#note" },
    { include: "#caps" },
    { include: "#property" },
    { include: "#pad" },
    { include: "#element" },
    { include: "#link" },
  ],
  repository: {
    // [per track], and (prose in parentheses) that doesn't hang off an element name.
    note: {
      patterns: [
        { name: "comment.block.note.gst", match: "\\[[^\\]]*\\]" },
        { name: "comment.block.note.gst", match: "(?<![\\w*}])\\([^)]*\\)" },
      ],
    },
    // video/x-raw(memory:DMABuf),format=NV12,width=1280 — the media type, then its fields.
    caps: {
      match: "\\b([a-z]+/[A-Za-z0-9.+-]+)(\\([^)]*\\))?",
      captures: {
        1: { name: "entity.name.type.caps.gst" },
        2: { name: "storage.modifier.caps-feature.gst" },
      },
    },
    property: {
      match: "(?<=^|[\\s,(])([A-Za-z][\\w-]*)(=)(\"(?:[^\"\\\\]|\\\\.)*\"|'[^']*'|[^\\s!,()\\[\\]]+)",
      captures: {
        1: { name: "entity.other.attribute-name.gst" },
        2: { name: "punctuation.separator.key-value.gst" },
        3: value,
      },
    },
    // webrtcbin.sink_%u — an element's pad.
    pad: {
      match: "\\b([A-Za-z][\\w-]*)(\\.)([A-Za-z_][\\w%]*)",
      captures: {
        1: { name: "entity.name.tag.element.gst" },
        2: { name: "punctuation.accessor.gst" },
        3: { name: "entity.name.function.pad.gst" },
      },
    },
    // An element name, with the shorthand's wildcards (rtp*pay) and alternatives (rtp{h264,vp8}pay),
    // optionally followed by the shorthand's property list: queue(leaky=downstream, max 1 GOP).
    element: {
      patterns: [
        {
          begin: "\\b((?:[A-Za-z0-9_*-]|\\{[^}]*\\})+)(\\()",
          beginCaptures: {
            1: { name: "entity.name.tag.element.gst", patterns: [{ include: "#alternatives" }] },
            2: { name: "punctuation.section.parameters.begin.gst" },
          },
          end: "\\)",
          endCaptures: { 0: { name: "punctuation.section.parameters.end.gst" } },
          patterns: [
            { include: "#property" },
            { name: "constant.numeric.gst", match: "\\b\\d+(?:[./]\\d+)?\\b" },
            { name: "entity.other.attribute-name.gst", match: "[A-Za-z][\\w-]*" },
            { name: "punctuation.separator.gst", match: "," },
          ],
        },
        {
          match: "(?<![\\w/.-])((?:[A-Za-z_*]|\\{[^}]*\\})(?:[A-Za-z0-9_*-]|\\{[^}]*\\})*)",
          captures: { 1: { name: "entity.name.tag.element.gst", patterns: [{ include: "#alternatives" }] } },
        },
      ],
    },
    alternatives: {
      match: "\\{[^}]*\\}",
      name: "constant.other.alternatives.gst",
    },
    // The link operator, and the box-drawing arrows the docs use between stages.
    link: {
      patterns: [
        { name: "keyword.operator.link.gst", match: "!" },
        { name: "keyword.operator.arrow.gst", match: "[─━]*[►▶→]|[─━]+" },
        { name: "punctuation.separator.gst", match: "," },
      ],
    },
  },
};
