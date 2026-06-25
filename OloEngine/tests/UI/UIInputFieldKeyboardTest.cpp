#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// UIInputFieldKeyboardTest — unit test for UI text-field keyboard handling.
//
// Subsystem under test: UIInputSystem::ProcessInput's keyboard path, which
// edits the focused UIInputFieldComponent — inserting typed characters at the
// cursor, backspace/delete, codepoint-wise cursor movement, character-limit
// enforcement — and keeps m_CursorPosition (a byte offset into the UTF-8
// m_Text) on codepoint boundaries. Synthetic UIKeyboardInput drives the
// system directly, so no window/Application is required.
//
// Also covers the Input typed-character double buffer (OnCharTyped / Update /
// GetTypedCharacters) that feeds the real runtime call site in Scene.cpp.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Input.h"
#include "OloEngine/Core/UTF8.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/UI/UIInputSystem.h"

#include <string>

using namespace OloEngine; // NOLINT(google-build-using-namespace)

namespace
{
    // Build a UIKeyboardInput that types the ASCII characters of `text`.
    UIKeyboardInput TypeAscii(const std::string& text)
    {
        UIKeyboardInput kb;
        for (const char c : text)
        {
            kb.m_TypedCharacters.push_back(static_cast<u32>(static_cast<unsigned char>(c)));
        }
        return kb;
    }
} // namespace

class UIInputFieldKeyboardTest : public ::testing::Test
{
  protected:
    Ref<Scene> m_Scene;
    Entity m_Field;

    void SetUp() override
    {
        m_Scene = Scene::Create();
        m_Field = m_Scene->CreateEntity("InputField");
        m_Field.AddComponent<UIInputFieldComponent>();
        m_Field.AddComponent<UIResolvedRectComponent>();
    }

    void TearDown() override
    {
        m_Scene = nullptr;
    }

    UIInputFieldComponent& Field()
    {
        return m_Field.GetComponent<UIInputFieldComponent>();
    }

    UIResolvedRectComponent& Rect()
    {
        return m_Field.GetComponent<UIResolvedRectComponent>();
    }

    // Run a keyboard-only ProcessInput pass (no mouse activity).
    void Process(const UIKeyboardInput& kb)
    {
        UIInputSystem::ProcessInput(*m_Scene, { 0.0f, 0.0f }, false, false, 0.0f, 0.0f, kb);
    }
};

// --- Insertion -------------------------------------------------------------

TEST_F(UIInputFieldKeyboardTest, TypingIntoFocusedFieldInsertsTextAndAdvancesCursor)
{
    Field().m_IsFocused = true;
    Process(TypeAscii("Hello"));
    EXPECT_EQ(Field().m_Text, "Hello");
    EXPECT_EQ(Field().m_CursorPosition, 5);
}

TEST_F(UIInputFieldKeyboardTest, TypingInsertsAtCursorInMiddle)
{
    auto& f = Field();
    f.m_IsFocused = true;
    Process(TypeAscii("ac"));

    UIKeyboardInput left;
    left.m_CursorLeft = true;
    Process(left); // cursor now sits before 'c'

    Process(TypeAscii("b"));
    EXPECT_EQ(f.m_Text, "abc");
    EXPECT_EQ(f.m_CursorPosition, 2);
}

TEST_F(UIInputFieldKeyboardTest, ControlCharactersAreIgnored)
{
    auto& f = Field();
    f.m_IsFocused = true;

    UIKeyboardInput kb;
    kb.m_TypedCharacters = { static_cast<u32>('a'), 0x08u /*BS*/, 0x0Au /*LF*/, 0x7Fu /*DEL*/, static_cast<u32>('b') };
    Process(kb);

    EXPECT_EQ(f.m_Text, "ab");
    EXPECT_EQ(f.m_CursorPosition, 2);
}

// --- Focus gating ----------------------------------------------------------

TEST_F(UIInputFieldKeyboardTest, UnfocusedFieldIgnoresTypedText)
{
    Field().m_IsFocused = false;
    Process(TypeAscii("Hello"));
    EXPECT_TRUE(Field().m_Text.empty());
    EXPECT_EQ(Field().m_CursorPosition, 0);
}

TEST_F(UIInputFieldKeyboardTest, NonInteractableFieldIsBlurredAndIgnoresInput)
{
    auto& f = Field();
    f.m_IsFocused = true;
    f.m_Interactable = false;
    Process(TypeAscii("Hello"));
    EXPECT_FALSE(f.m_IsFocused);
    EXPECT_TRUE(f.m_Text.empty());
}

// --- Backspace / Delete ----------------------------------------------------

TEST_F(UIInputFieldKeyboardTest, BackspaceRemovesCodepointBeforeCursor)
{
    auto& f = Field();
    f.m_IsFocused = true;
    Process(TypeAscii("abc")); // cursor at 3

    UIKeyboardInput kb;
    kb.m_Backspace = true;
    Process(kb);

    EXPECT_EQ(f.m_Text, "ab");
    EXPECT_EQ(f.m_CursorPosition, 2);
}

TEST_F(UIInputFieldKeyboardTest, BackspaceAtStartIsNoOp)
{
    auto& f = Field();
    f.m_IsFocused = true;
    Process(TypeAscii("ab"));

    UIKeyboardInput home;
    home.m_Home = true;
    Process(home); // cursor at 0

    UIKeyboardInput kb;
    kb.m_Backspace = true;
    Process(kb);

    EXPECT_EQ(f.m_Text, "ab");
    EXPECT_EQ(f.m_CursorPosition, 0);
}

TEST_F(UIInputFieldKeyboardTest, DeleteRemovesCodepointAtCursor)
{
    auto& f = Field();
    f.m_IsFocused = true;
    Process(TypeAscii("abc"));

    UIKeyboardInput home;
    home.m_Home = true;
    Process(home); // cursor at 0

    UIKeyboardInput del;
    del.m_Delete = true;
    Process(del);

    EXPECT_EQ(f.m_Text, "bc");
    EXPECT_EQ(f.m_CursorPosition, 0);
}

TEST_F(UIInputFieldKeyboardTest, DeleteAtEndIsNoOp)
{
    auto& f = Field();
    f.m_IsFocused = true;
    Process(TypeAscii("ab")); // cursor at end (2)

    UIKeyboardInput del;
    del.m_Delete = true;
    Process(del);

    EXPECT_EQ(f.m_Text, "ab");
    EXPECT_EQ(f.m_CursorPosition, 2);
}

// --- Cursor movement -------------------------------------------------------

TEST_F(UIInputFieldKeyboardTest, CursorLeftRightMoveByOneCodepoint)
{
    auto& f = Field();
    f.m_IsFocused = true;
    Process(TypeAscii("abc")); // cursor 3

    UIKeyboardInput left;
    left.m_CursorLeft = true;
    Process(left);
    EXPECT_EQ(f.m_CursorPosition, 2);
    Process(left);
    EXPECT_EQ(f.m_CursorPosition, 1);

    UIKeyboardInput right;
    right.m_CursorRight = true;
    Process(right);
    EXPECT_EQ(f.m_CursorPosition, 2);

    EXPECT_EQ(f.m_Text, "abc"); // movement never edits text
}

TEST_F(UIInputFieldKeyboardTest, CursorClampsAtBothEnds)
{
    auto& f = Field();
    f.m_IsFocused = true;
    Process(TypeAscii("ab")); // cursor at end (2)

    UIKeyboardInput right;
    right.m_CursorRight = true;
    Process(right);
    EXPECT_EQ(f.m_CursorPosition, 2); // can't move past the end

    UIKeyboardInput home;
    home.m_Home = true;
    Process(home);

    UIKeyboardInput left;
    left.m_CursorLeft = true;
    Process(left);
    EXPECT_EQ(f.m_CursorPosition, 0); // can't move before the start
}

TEST_F(UIInputFieldKeyboardTest, HomeAndEndJumpToBounds)
{
    auto& f = Field();
    f.m_IsFocused = true;
    Process(TypeAscii("abcd"));

    UIKeyboardInput home;
    home.m_Home = true;
    Process(home);
    EXPECT_EQ(f.m_CursorPosition, 0);

    UIKeyboardInput end;
    end.m_End = true;
    Process(end);
    EXPECT_EQ(f.m_CursorPosition, 4);
}

// --- Character limit -------------------------------------------------------

TEST_F(UIInputFieldKeyboardTest, CharacterLimitIsEnforced)
{
    auto& f = Field();
    f.m_IsFocused = true;
    f.m_CharacterLimit = 3;
    Process(TypeAscii("abcdef"));
    EXPECT_EQ(f.m_Text, "abc");
    EXPECT_EQ(f.m_CursorPosition, 3);
}

TEST_F(UIInputFieldKeyboardTest, ZeroCharacterLimitMeansUnlimited)
{
    auto& f = Field();
    f.m_IsFocused = true;
    f.m_CharacterLimit = 0;
    Process(TypeAscii("abcdefghij"));
    EXPECT_EQ(f.m_Text.size(), 10u);
}

// --- UTF-8 (multi-byte) ----------------------------------------------------

TEST_F(UIInputFieldKeyboardTest, MultibyteCodepointInsertedAndBackspacedAtomically)
{
    auto& f = Field();
    f.m_IsFocused = true;

    UIKeyboardInput kb;
    kb.m_TypedCharacters = { 0x00E9u }; // 'é' — 2 bytes in UTF-8
    Process(kb);
    EXPECT_EQ(f.m_Text.size(), 2u);
    EXPECT_EQ(f.m_CursorPosition, 2);

    UIKeyboardInput bs;
    bs.m_Backspace = true;
    Process(bs); // removes the whole 2-byte codepoint, not half of it
    EXPECT_TRUE(f.m_Text.empty());
    EXPECT_EQ(f.m_CursorPosition, 0);
}

TEST_F(UIInputFieldKeyboardTest, CursorMovesOverMultibyteCodepointAtomically)
{
    auto& f = Field();
    f.m_IsFocused = true;

    UIKeyboardInput kb;
    kb.m_TypedCharacters = { static_cast<u32>('a'), 0x4E2Du /* 中, 3 bytes */, static_cast<u32>('b') };
    Process(kb);
    EXPECT_EQ(f.m_Text.size(), 5u); // 1 + 3 + 1
    EXPECT_EQ(f.m_CursorPosition, 5);

    UIKeyboardInput left;
    left.m_CursorLeft = true;
    Process(left);
    EXPECT_EQ(f.m_CursorPosition, 4); // before 'b'
    Process(left);
    EXPECT_EQ(f.m_CursorPosition, 1); // before 中 — skipped all 3 bytes
    Process(left);
    EXPECT_EQ(f.m_CursorPosition, 0); // before 'a'
}

TEST_F(UIInputFieldKeyboardTest, CharacterLimitCountsMultibyteAsOneCharacter)
{
    auto& f = Field();
    f.m_IsFocused = true;
    f.m_CharacterLimit = 2;

    UIKeyboardInput kb;
    kb.m_TypedCharacters = { 0x00E9u, 0x00E9u, 0x00E9u }; // three 'é'
    Process(kb);

    EXPECT_EQ(UTF8::CountCodepoints(f.m_Text), 2u); // limited to 2 codepoints
    EXPECT_EQ(f.m_Text.size(), 4u);                 // 2 codepoints * 2 bytes
}

TEST_F(UIInputFieldKeyboardTest, CursorMidCodepointIsSnappedToBoundaryBeforeEditing)
{
    auto& f = Field();
    f.m_IsFocused = true;
    // External mutation (e.g. a script assigning m_Text) can leave the byte
    // cursor pointing inside a multi-byte codepoint.
    f.m_Text = "a\xC3\xA9"; // 'a' + 'é' (é = 2 bytes: 0xC3 0xA9)
    f.m_CursorPosition = 2; // points at é's continuation byte — mid-codepoint

    UIKeyboardInput bs;
    bs.m_Backspace = true;
    Process(bs);

    // The cursor snaps back onto é's boundary (byte 1) before editing, so
    // backspace removes the whole 'a' before it and leaves 'é' well-formed —
    // never an orphaned 0xA9 continuation byte.
    EXPECT_EQ(f.m_Text, "\xC3\xA9"); // "é"
    EXPECT_EQ(f.m_CursorPosition, 0);
}

// --- Focus via click then edit ---------------------------------------------

TEST_F(UIInputFieldKeyboardTest, ClickFocusesFieldThenKeyboardEditsSameFrame)
{
    auto& f = Field();
    Rect().m_Position = { 10.0f, 10.0f };
    Rect().m_Size = { 100.0f, 30.0f };
    ASSERT_FALSE(f.m_IsFocused);

    // Press inside the rect, with a typed character in the same frame.
    UIKeyboardInput kb = TypeAscii("x");
    UIInputSystem::ProcessInput(*m_Scene, { 20.0f, 20.0f }, true, true, 0.0f, 0.0f, kb);

    EXPECT_TRUE(f.m_IsFocused);
    EXPECT_EQ(f.m_Text, "x");
}

TEST_F(UIInputFieldKeyboardTest, ClickOutsideBlursAndStopsEditingSameFrame)
{
    auto& f = Field();
    Rect().m_Position = { 10.0f, 10.0f };
    Rect().m_Size = { 100.0f, 30.0f };
    f.m_IsFocused = true;

    // Press outside the rect; the field blurs first, so the typed char is dropped.
    UIKeyboardInput kb = TypeAscii("x");
    UIInputSystem::ProcessInput(*m_Scene, { 500.0f, 500.0f }, true, true, 0.0f, 0.0f, kb);

    EXPECT_FALSE(f.m_IsFocused);
    EXPECT_TRUE(f.m_Text.empty());
}

// --- Cursor clamps when text mutated externally ----------------------------

TEST_F(UIInputFieldKeyboardTest, OutOfRangeCursorIsClampedBeforeEditing)
{
    auto& f = Field();
    f.m_IsFocused = true;
    f.m_Text = "ab";
    f.m_CursorPosition = 99; // e.g. left stale by deserialization / scripting

    UIKeyboardInput bs;
    bs.m_Backspace = true;
    Process(bs);

    EXPECT_EQ(f.m_Text, "a"); // clamped to end, then deleted the last codepoint
    EXPECT_EQ(f.m_CursorPosition, 1);
}

// --- Input typed-character buffer (runtime data path) ----------------------

TEST(UIInputBufferTest, TypedCharacterBufferRotatesPerUpdate)
{
    // Drain any state left by earlier ticks in this process.
    Input::Update();
    Input::Update();
    EXPECT_TRUE(Input::GetTypedCharacters().empty());

    Input::OnCharTyped(static_cast<u32>('A'));
    Input::OnCharTyped(static_cast<u32>('B'));
    // Pending chars aren't visible until Update() rotates the buffer.
    EXPECT_TRUE(Input::GetTypedCharacters().empty());

    Input::Update();
    ASSERT_EQ(Input::GetTypedCharacters().size(), 2u);
    EXPECT_EQ(Input::GetTypedCharacters()[0], static_cast<u32>('A'));
    EXPECT_EQ(Input::GetTypedCharacters()[1], static_cast<u32>('B'));

    // A frame with no typed input clears it again.
    Input::Update();
    EXPECT_TRUE(Input::GetTypedCharacters().empty());
}
