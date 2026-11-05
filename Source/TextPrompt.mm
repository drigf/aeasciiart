// TextPrompt.mm

#include "TextPrompt.h"

#import <Cocoa/Cocoa.h>

static bool promptOnMainThread(const char* title, const char* info, const std::string& current, std::string& result);

bool PromptForText(const char* title, const char* info, const std::string& current, std::string& result)
{
    // Cocoa UI must run on the main thread; After Effects may call us from another one.
    if ([NSThread isMainThread])
        return promptOnMainThread(title, info, current, result);

    __block bool ok = false;
    dispatch_sync(dispatch_get_main_queue(), ^{ ok = promptOnMainThread(title, info, current, result); });
    return ok;
}

static bool promptOnMainThread(const char* title, const char* info, const std::string& current, std::string& result)
{
    @autoreleasepool
    {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = [NSString stringWithUTF8String:title];
        alert.informativeText = [NSString stringWithUTF8String:info];
        [alert addButtonWithTitle:@"OK"];
        [alert addButtonWithTitle:@"Cancel"];

        NSTextField* field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 380, 24)];
        field.stringValue = [NSString stringWithUTF8String:current.c_str()] ?: @"";
        field.placeholderString = @" .:-=+*#%@";
        alert.accessoryView = field;
        alert.window.initialFirstResponder = field;

        // After Effects owns the foreground; without this the dialog can open behind its windows.
        [NSApp activateIgnoringOtherApps:YES];
        [alert.window setLevel:NSModalPanelWindowLevel];
        [alert.window makeKeyAndOrderFront:nil];

        if ([alert runModal] != NSAlertFirstButtonReturn)
            return false;

        const char* utf8 = field.stringValue.UTF8String;
        result = utf8 ? utf8 : "";
        return true;
    }
}

bool PromptForCharacters(const std::string& current, std::string& result)
{
    return PromptForText(
        "Custom ASCII Characters",
        "Type the characters to draw with. Order doesn't matter: they're automatically sorted from light to dark "
        "by how much ink each one uses, and repeats are ignored. Letters, symbols and emoji all work. "
        "Try a word, like \"ASCII\", or a ramp, like \" .oO0@\".",
        current,
        result);
}
