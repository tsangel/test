# Troubleshooting

Use this page when the first build, read, decode, or write attempt fails and you need the shortest path to the likely cause.

## Common failure patterns

- wheel build fails before compilation:
  check Python, `pip`, `cmake`, compiler toolchain, and the active virtual environment
- later-tag mutation raises on a partially loaded file:
  load more of the file first, or avoid mutating data elements that have not been parsed yet
- `decode_into()` complains about shape, dtype, or buffer size:
  re-check rows, cols, samples per pixel, frame count, and output itemsize
- charset rewrite fails or replacement occurs:
  review the declared target charset and encode error policy
- tag/path lookup does not resolve:
  confirm the keyword spelling or dotted path form

## Where to look next

- read/decode failures: [Error Handling](error_handling.md)
- nested path issues: [Sequence and Paths](sequence_and_paths.md)
- pixel encode issues: [Pixel Encode Constraints](../reference/pixel_encode_constraints.md)
- exact failure categories: [Error Model](../reference/error_model.md)
