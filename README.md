Multi-platform Builder
=============

[![CodeQL](https://github.com/Next2D/builder/actions/workflows/codeql-analysis.yml/badge.svg?branch=main)](https://github.com/Next2D/builder/actions/workflows/codeql-analysis.yml)
[![Lint](https://github.com/Next2D/builder/actions/workflows/lint.yml/badge.svg?branch=main)](https://github.com/Next2D/builder/actions/workflows/lint.yml)
[![release](https://img.shields.io/github/v/release/Next2D/builder)](https://github.com/Next2D/builder/releases)
[![license](https://img.shields.io/github/license/Next2D/builder)](https://github.com/Next2D/builder/blob/main/LICENSE)

[日本語]\
Next2D Frameworkのマルチプラットフォームビルダー、macOS、Windows、iOS、Android、Web（HTML）など、各種プラットフォームへの書き出しに対応

[English]\
Next2D Framework multi-platform builder, supports export to various platforms including macOS, Windows, iOS, Android, and Web (HTML)

## Supported

| platform | detail                   |
|----------|--------------------------|
| macOS    | Export dmg file          |
| Windows  | Export an exe file       |
| iOS      | Implementing...          |
| Android  | Implementing...          |
| web      | Export minfy'd JS files  |

### build example.

```linux
npx @next2d/builder --platform web --env prd
```

### debug example.

```linux
npx @next2d/builder --debug --platform web --env prd
```

## License
This project is licensed under the [MIT License](https://opensource.org/licenses/MIT) - see the [LICENSE](LICENSE) file for details.