// TextPrompt.h
//
// A tiny native dialog for typing the custom character set.  Implemented in
// TextPrompt.mm (Cocoa); callable from plain C++.

#pragma once

#include <string>

// Shows a modal "type your characters" dialog pre-filled with `current` (UTF-8).
// Returns true and fills `result` if the user pressed OK; false if cancelled.
// `title` and `info` are the dialog's heading and explanatory text (UTF-8).
bool PromptForText(const char* title, const char* info, const std::string& current, std::string& result);

// Convenience wrapper for the custom character set dialog.
bool PromptForCharacters(const std::string& current, std::string& result);
