# Contributing

Issues and pull requests are welcome.

- Keep changes small and focused; one topic per pull request.
- `make test` must pass (it runs with AddressSanitizer and UndefinedBehaviorSanitizer). Add a
  host test for new behaviour where you can.
- Follow the style in [docs/kernel.md](docs/kernel.md#style): the Linux kernel coding style
  (run `make format`), `module_verb` names, guard clauses first, module-private code `static`,
  no hidden allocation. Document every public function in its header (Doxygen comments).
- The kernel must keep working on 16-bit MCUs with a few KB of RAM: no recursion, no large stack
  buffers, sizes as named constants.
- New files start with:

  ```c
  /* SPDX-License-Identifier: Apache-2.0 */
  /* Copyright (c) <year> <you> */
  ```

By contributing you agree that your contribution is licensed under the Apache License 2.0
(see section 5 of LICENSE).
