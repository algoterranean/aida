#pragma once

#include "CoreMinimal.h"

/**
 * Convert the subset of Markdown that AIDA/LLMs actually emit into UMG RichTextBlock markup, so chat
 * replies render with formatting instead of raw `**` / `#` noise. Supported: **bold**, *italic*, `code`,
 * # headers, - / * bullets, and ``` fenced code blocks. Emits the style tags <Bold>, <Italic>, <Code>,
 * <Header>; untagged text uses the block's default style (the widget supplies matching styles).
 *
 * Pure + unit-tested. Best-effort ("as close as we can"), not a full CommonMark parser: unbalanced markers
 * are left as literal text, and `_`/`__` are intentionally ignored to avoid snake_case false positives.
 */
FString AIDAMarkdownToRichText(const FString& Markdown);
