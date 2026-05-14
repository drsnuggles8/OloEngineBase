# Unreal Engine 5.7 — Testing Architecture & Framework Guide

> A comprehensive overview of how UE5 tests the engine itself, and how it supports
> testing for games built on top of it. Written for engine developers who want to
> understand — and potentially replicate — UE's testing philosophy.

---

## Table of Contents

1. [High-Level Overview](#1-high-level-overview)
2. [The Multi-Layer Testing Pyramid](#2-the-multi-layer-testing-pyramid)
3. [Layer 1 — Low-Level Tests (Catch2)](#3-layer-1--low-level-tests-catch2)
4. [Layer 2 — Automation Test Framework (Core)](#4-layer-2--automation-test-framework-core)
5. [Layer 3 — CQTest (Modern Fixture Framework)](#5-layer-3--cqtest-modern-fixture-framework)
6. [Layer 4 — Spec / BDD Tests](#6-layer-4--spec--bdd-tests)
7. [Layer 5 — Functional Tests (Map/Actor-Based)](#7-layer-5--functional-tests-mapactor-based)
8. [Layer 6 — Screenshot & Visual Regression Tests](#8-layer-6--screenshot--visual-regression-tests)
9. [Layer 7 — UI Automation Driver](#9-layer-7--ui-automation-driver)
10. [Layer 8 — Python Automation Tests](#10-layer-8--python-automation-tests)
11. [Layer 9 — Gauntlet (Distributed CI/CD Testing)](#11-layer-9--gauntlet-distributed-cicd-testing)
12. [Editor Test Runner UI](#12-editor-test-runner-ui)
13. [Command-Line Test Execution](#13-command-line-test-execution)
14. [Test Taxonomy — Flags, Priorities & Filters](#14-test-taxonomy--flags-priorities--filters)
15. [Latent Commands — Multi-Frame Test Execution](#15-latent-commands--multi-frame-test-execution)
16. [Testing Plugins & Modules Map](#16-testing-plugins--modules-map)
17. [Key Source File Reference](#17-key-source-file-reference)
18. [Architecture Diagram](#18-architecture-diagram)
19. [Lessons for Custom Engine Developers](#19-lessons-for-custom-engine-developers)

---

## 1. High-Level Overview

Unreal Engine has a **deeply layered** testing infrastructure. Unlike many game engines
that bolt on testing as an afterthought, UE treats testing as a first-class system — 
integrated into the runtime, editor, build pipeline, and CI/CD infrastructure.

**Three distinct audiences** are served:

| Audience | What they test | Primary tools |
|----------|---------------|---------------|
| **Epic engine developers** | Core C++ engine systems | Low-Level Tests (Catch2), Automation Tests, CQTest |
| **Game developers (C++)** | Gameplay code, systems | Automation Tests, CQTest, Functional Tests, Specs |
| **Game developers (Blueprint)** | Gameplay logic, levels | Functional Test actors, Screenshot Tests, Python Tests |

**Key design principle**: Tests live *alongside* the code they test, registered globally
via macros, discovered automatically at startup, and run through a unified framework
that works identically in-editor, from the command line, and across distributed CI farms.

---

## 2. The Multi-Layer Testing Pyramid

```
                    ┌────────────────────-─────┐
                    │   Gauntlet / Horde CI    │  ← Orchestration
                    ├──────────────-───────────┤
                    │  Screenshot / Visual     │  ← Visual regression
                    ├─────────────-────────────┤
                    │  Functional Tests        │  ← Map-based integration
                    │  (Actor-based, BP-able)  │
                    ├───────────────-──────────┤
                    │  BDD Specs (DEFINE_SPEC) │  ← Behavior-driven
                    ├───────────-──────────────┤
                    │  CQTest (Fixture-based)  │  ← Modern xUnit-style
                    ├────────────-─────────────┤
                    │  Automation Tests        │  ← Core framework
                    │  (Simple + Complex)      │
                    ├──────────-───────────────┤
                    │  Low-Level Tests (Catch2)│  ← Fast C++ unit tests
                    └─────────-────────────────┘
                    
          Each layer builds on the ones below it.
```

---

## 3. Layer 1 — Low-Level Tests (Catch2)

**Source**: `Engine/Source/Developer/LowLevelTestsRunner/`  
**Framework**: [Catch2](https://github.com/catchorg/Catch2) (modern C++ test framework)

### Purpose

The fastest, most isolated tests. These run as **standalone executables** — no editor,
no engine loop, no rendering. Pure C++ unit tests for core algorithms, containers,
math, serialization, etc.

### Writing a Test

```cpp
#include <catch2/catch_test_macros.hpp>

TEST_CASE("FString::Append works correctly", "[Core][String]")
{
    FString S = TEXT("Hello");
    
    SECTION("Appending a string")
    {
        S.Append(TEXT(" World"));
        REQUIRE(S == TEXT("Hello World"));
    }
    
    SECTION("Appending empty string")
    {
        S.Append(TEXT(""));
        REQUIRE(S == TEXT("Hello"));
    }
}
```

### Test Types Supported

| Type | Description |
|------|-------------|
| **Unit** | Single function/class in isolation |
| **Integration** | Multiple components working together |
| **Functional** | Feature from user perspective |
| **Smoke** | Quick sanity checks |
| **Performance** | Benchmarks with `BENCHMARK` macro |
| **Stress** | Load/endurance testing |

### Two Modes of Operation

1. **Explicit Tests**: Separate module + target (`.Build.cs` + `.Target.cs`). The test
   is its own standalone executable. Best for core subsystems.

2. **Implicit Tests**: Test code lives inside the module being tested (in a `Tests/`
   subfolder). No separate target needed. Faster iteration but couples test to module.

### Running

```bash
# Via AutomationTool
RunUAT.bat RunLowLevelTests -testapp=CoreTests -platform=Win64

# Direct executable
CoreTests.exe --reporter=xml --out=results.xml
```

### Key Files

- `Engine/Source/Developer/LowLevelTestsRunner/README.md` — Full framework docs
- `Engine/Source/Developer/LowLevelTestsRunner/Public/` — Runner public API
- `Engine/Source/Programs/AutomationTool/LowLevelTests/RunLowLevelTests.cs` — CI runner

---

## 4. Layer 2 — Automation Test Framework (Core)

**Source**: `Engine/Source/Runtime/Core/Public/Misc/AutomationTest.h` (~4600 lines)  
**Implementation**: `Engine/Source/Runtime/Core/Private/Misc/AutomationTest.cpp`

This is the **backbone** of UE testing. Every other test framework (CQTest, Specs,
Functional Tests) ultimately registers with this system.

### Core Architecture

```
┌──────────────────────────────────────────────┐
│          FAutomationTestFramework            │  Singleton
│  ┌────────────────────────────────────────┐  │
│  │  Registry (TMap<FString, TestClass*>)  │  │
│  │  ┌──────────┐ ┌──────────┐ ┌────────┐ │  │
│  │  │ TestA    │ │ TestB    │ │ TestC  │ │  │
│  │  └──────────┘ └──────────┘ └────────┘ │  │
│  └────────────────────────────────────────┘  │
│                                              │
│  Latent Command Queue ──────────────────►    │
│  Network Command Queue ─────────────────►    │
│  Event Broadcasts (Start/End/Pre/Post) ──►   │
└──────────────────────────────────────────────┘
```

### Key Classes

| Class | Role |
|-------|------|
| `FAutomationTestFramework` | Singleton — registers, discovers, executes all tests |
| `FAutomationTestBase` | Abstract base class every test inherits from |
| `FAutomationTestInfo` | Metadata: name, path, flags, source location |
| `FAutomationTestExecutionInfo` | Results container: events, telemetry, errors |
| `IAutomationLatentCommand` | Multi-frame deferred command interface |
| `IAutomationNetworkCommand` | Multi-participant distributed test command |

### Simple Test

The most common pattern — a single `RunTest()` method:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMathVectorTest,                    // Class name
    "System.Math.Vector.Addition",      // Hierarchical test path
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FMathVectorTest::RunTest(const FString& Parameters)
{
    FVector A(1, 2, 3);
    FVector B(4, 5, 6);
    FVector Result = A + B;
    
    TestEqual(TEXT("X component"), Result.X, 5.0f);
    TestEqual(TEXT("Y component"), Result.Y, 7.0f);
    TestEqual(TEXT("Z component"), Result.Z, 9.0f);
    
    return !HasAnyErrors();
}
```

### Complex Test (Parameterized)

Generates multiple test variants from data:

```cpp
IMPLEMENT_COMPLEX_AUTOMATION_TEST(
    FMapLoadTest,
    "Project.Maps.LoadAll",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

void FMapLoadTest::GetTests(
    TArray<FString>& OutBeautifiedNames,
    TArray<FString>& OutTestCommands) const
{
    // Enumerate all maps in the project
    TArray<FString> MapFiles;
    IFileManager::Get().FindFilesRecursive(MapFiles, *FPaths::ProjectContentDir(),
        TEXT("*.umap"), true, false);
    
    for (const FString& Map : MapFiles)
    {
        OutBeautifiedNames.Add(FPaths::GetBaseFilename(Map));
        OutTestCommands.Add(Map);
    }
}

bool FMapLoadTest::RunTest(const FString& Parameters)
{
    // Parameters = the map path for this variant
    ADD_LATENT_AUTOMATION_COMMAND(FLoadMapCommand(Parameters));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitForMapLoadCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FValidateMapCommand());
    return true;
}
```

### Network Test (Multi-Participant)

```cpp
IMPLEMENT_NETWORKED_AUTOMATION_TEST(
    FReplicationTest,
    "System.Networking.Replication",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter,
    2  // Number of participants required
)
```

### Assertion Methods

```cpp
TestEqual(Description, Actual, Expected)    // ==
TestNotEqual(Description, Actual, Expected) // !=
TestTrue(Description, Value)                // bool check
TestFalse(Description, Value)
TestNull(Description, Ptr)
TestNotNull(Description, Ptr)
TestSame(Description, A, B)                 // Same reference
TestLessThan(Description, A, B)             // <
TestGreaterThan(Description, A, B)          // >
TestNearlyEqual(Description, A, B, Tol)     // Floating point ≈

// One-liner macros (return false on failure — good for early exit)
UTEST_EQUAL("Check", Actual, Expected)
UTEST_TRUE("Condition", Value)
UTEST_NOT_NULL("Pointer", Ptr)
```

### Expected Message Validation

Tests can declare that specific log messages are *expected* — the test fails if those
messages don't appear:

```cpp
AddExpectedMessage(
    TEXT("Asset not found"),            // Pattern
    ELogVerbosity::Warning,             // Required verbosity
    EAutomationExpectedMessageFlags::Contains,  // Match type
    1                                   // Expected occurrences
);

// ... test code that should trigger the warning ...
```

### Test Registration Flow

```
1. IMPLEMENT_SIMPLE_AUTOMATION_TEST macro expands to:
   - A class definition inheriting FAutomationTestBase
   - A global static instance of that class
   
2. The constructor calls:
   FAutomationTestFramework::Get().RegisterAutomationTest(Name, this)
   
3. The framework stores it in a TMap keyed by test path
   
4. When the editor's Automation tab opens (or CLI requests),
   the framework enumerates all registered tests
```

---

## 5. Layer 3 — CQTest (Modern Fixture Framework)

**Source**: `Engine/Source/Developer/CQTest/`  
**Plugin**: `Engine/Plugins/Tests/CQTest/`

CQTest is UE's **modern xUnit-style** testing framework — cleaner syntax with
proper fixtures, setup/teardown, and tagging. It wraps the Automation Test Framework
underneath.

### Simple Test

```cpp
#include "CQTest.h"

TEST(MySimpleTest, "Game.Combat.DamageCalculation")
{
    float Damage = CalculateDamage(100.0f, 0.5f);
    ASSERT_THAT(IsEqual(Damage, 50.0f));
}
```

### Fixture-Based Test (xUnit Pattern)

```cpp
TEST_CLASS(InventoryTests, "Game.Inventory")
{
    UInventoryComponent* Inventory;
    AActor* TestActor;
    
    BEFORE_EACH()
    {
        TestActor = SpawnTestActor();
        Inventory = TestActor->FindComponentByClass<UInventoryComponent>();
    }
    
    AFTER_EACH()
    {
        TestActor->Destroy();
    }
    
    TEST_METHOD(AddItem_IncreasesCount)
    {
        Inventory->AddItem(FItemData("Sword", 1));
        ASSERT_THAT(IsEqual(Inventory->GetItemCount(), 1));
    }
    
    TEST_METHOD(RemoveItem_DecreasesCount)
    {
        Inventory->AddItem(FItemData("Sword", 1));
        Inventory->RemoveItem("Sword");
        ASSERT_THAT(IsEqual(Inventory->GetItemCount(), 0));
    }
};
```

### Available Macros

| Macro | Purpose |
|-------|---------|
| `TEST(Name, Path)` | Simple single test |
| `TEST_CLASS(Name, Path)` | Fixture with shared state |
| `TEST_METHOD(Name)` | Test method inside a fixture |
| `BEFORE_EACH()` | Per-test setup (runs before each TEST_METHOD) |
| `AFTER_EACH()` | Per-test teardown |
| `BEFORE_ALL()` | One-time setup before all tests in class |
| `AFTER_ALL()` | One-time cleanup after all tests in class |
| `ASSERT_THAT(expr)` | Assertion with early return on failure |
| `TEST_WITH_TAGS(Name, Path, Tags)` | Test with tag metadata |
| `TEST_CLASS_WITH_FLAGS(Name, Path, Flags)` | Custom automation flags |
| `TEST_CLASS_WITH_BASE(Name, Path, Base)` | Custom base class |

---

## 6. Layer 4 — Spec / BDD Tests

**Defined in**: `AutomationTest.h` via `DEFINE_SPEC` / `BEGIN_DEFINE_SPEC`

This is UE's **Behavior-Driven Development** (BDD) framework, inspired by
RSpec/Jasmine/Jest. Tests read like specifications.

### Basic Spec

```cpp
BEGIN_DEFINE_SPEC(FXmlFileSpec, "System.Core.XmlFile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
    // Member variables declared here
    FXmlFile* XmlFile;
END_DEFINE_SPEC(FXmlFileSpec)

void FXmlFileSpec::Define()
{
    Describe("Parsing", [this]()
    {
        It("Should return no root node for an empty file", [this]()
        {
            XmlFile = new FXmlFile(FString(""), EConstructMethod::ConstructFromBuffer);
            TestNull("RootNode", XmlFile->GetRootNode());
        });
        
        It("Should parse single and double quoted attributes", [this]()
        {
            FString Xml = TEXT("<test a='val1' b=\"val2\"/>");
            XmlFile = new FXmlFile(Xml, EConstructMethod::ConstructFromBuffer);
            TestNotNull("RootNode", XmlFile->GetRootNode());
        });
    });
    
    Describe("Writing", [this]()
    {
        BeforeEach([this]()
        {
            XmlFile = new FXmlFile();
        });
        
        AfterEach([this]()
        {
            delete XmlFile;
            XmlFile = nullptr;
        });
        
        It("Should serialize to valid XML string", [this]()
        {
            // ...
        });
    });
}
```

### Features

| Feature | Syntax | Description |
|---------|--------|-------------|
| `Describe` | `Describe("context", []{})` | Group related tests |
| `It` | `It("should X", []{})` | Individual test case |
| `BeforeEach` | `BeforeEach([]{})` | Per-test setup |
| `AfterEach` | `AfterEach([]{})` | Per-test teardown |
| `LatentIt` | `LatentIt("name", [](FDoneDelegate Done){})` | Async test with done callback |
| `xDescribe` | `xDescribe("name", []{})` | Disabled group (skip) |
| `xIt` | `xIt("name", []{})` | Disabled test (skip) |

### Real-World Example (from UE codebase)

```cpp
BEGIN_DEFINE_SPEC(FVerifierSpec, "BuildPatchServices.Unit",
    EAutomationTestFlags::ProductFilter | EAutomationTestFlags_ApplicationContextMask)
    TUniquePtr<IVerifier> Verifier;
    TUniquePtr<FFakeFileSystem> FakeFileSystem;
END_DEFINE_SPEC(FVerifierSpec)

void FVerifierSpec::Define()
{
    BeforeEach([this]()
    {
        FakeFileSystem.Reset(new FFakeFileSystem());
    });
    
    Describe("Verify", [this]()
    {
        Describe("when SHA verifying all files", [this]()
        {
            BeforeEach([this]() { /* setup test files */ });
            
            It("should load and SHA check all files", [this]()
            {
                TArray<FString> OutdatedFiles;
                Verifier->Verify(OutdatedFiles);
                TestEqual(TEXT("No outdated files"), OutdatedFiles.Num(), 0);
            });
        });
    });
}
```

---

## 7. Layer 5 — Functional Tests (Map/Actor-Based)

**Source**: `Engine/Source/Developer/FunctionalTesting/`

Functional Tests are **actor-based tests that live in maps**. They're the bridge
between code testing and real gameplay testing. Crucially, they are **Blueprintable** —
designers and gameplay programmers can create tests without C++.

### Architecture

```
┌──────────────────────────────────────────┐
│              Test Map (.umap)            │
│  ┌────────────────┐ ┌────────────────┐  │
│  │ AFunctionalTest│ │ AFunctionalTest│  │
│  │ "CombatTest"   │ │ "AIPatrolTest" │  │
│  └────────────────┘ └────────────────┘  │
│                                          │
│  ┌─────────────────────────────────┐     │
│  │ AFunctionalTestGameMode         │     │
│  │ (discovers & runs all tests)    │     │
│  └─────────────────────────────────┘     │
└──────────────────────────────────────────┘
         │
         ▼
 FunctionalTestingManager
         │
         ▼
 FAutomationTestFramework (registered as automation tests)
```

### Test Lifecycle

```
1. PrepareTest()        — Initial setup (called once)
   ↓
2. IsReady()            — Polled each frame until true
   ↓
3. StartTest()          — Test begins executing
   ↓
4. [Test logic runs, possibly over multiple frames]
   ↓
5. FinishTest()         — Called by test code when done
   ↓
6. OnTestFinished()     — Cleanup callback
```

### C++ Functional Test

```cpp
UCLASS()
class AMyFunctionalTest : public AFunctionalTest
{
    GENERATED_BODY()
    
    virtual void StartTest() override
    {
        Super::StartTest();
        
        // Spawn enemy, do combat, check result
        AActor* Enemy = GetWorld()->SpawnActor<AEnemy>(SpawnPoint);
        // ...
        
        FinishTest(EFunctionalTestResult::Succeeded, "Combat works correctly");
    }
};
```

### Blueprint Functional Test

Blueprint subclasses of `AFunctionalTest` can override these events:

| Event | When |
|-------|------|
| `ReceivePrepareTest` | Called before IsReady() — setup |
| `ReceiveStartTest` | Called when test begins |
| `ReceiveTestFinished` | Called during cleanup |
| `IsReady` | Polled each frame — return true when ready |

**Properties available in Blueprint:**

- `Author` — responsible team/person  
- `Description` — what the test validates  
- `TestTags` — categorization (e.g., `[Graphics][prio0]`)
- `TimeLimit` — maximum execution time  
- `ObservationPoint` — camera to observe from  
- `RandomNumbersStream` — deterministic RNG seed

### Blueprint Utility Functions

`UAutomationBlueprintFunctionLibrary` provides Blueprint-callable helpers:

- `TakeAutomationScreenshot()` — async screenshot capture
- `TakeAutomationScreenshotAtCamera()` — from specific camera
- `TakeAutomationScreenshotOfUI()` — UI element capture
- `EnableStatGroup()` / `DisableStatGroup()` — performance stats
- `GetStatIncAverage()`, `GetStatIncMax()` — query stat data
- `AreAutomatedTestsRunning()` — check automation state
- `AutomationWaitForLoading()` — wait for streaming/loading

---

## 8. Layer 6 — Screenshot & Visual Regression Tests

**Source**: `Engine/Source/Developer/FunctionalTesting/Classes/ScreenshotFunctionalTest.h`

### How It Works

```
1. PrepareForScreenshot()
   ├── Resize viewport to screenshot resolution
   ├── Disable temporal AA (prevents frame-to-frame variation)
   └── Configure render environment
   
2. RequestScreenshot()
   └── Captures frame buffer pixels
   
3. OnScreenShotCaptured()
   ├── Compare against saved baseline image
   ├── Apply tolerance settings
   └── Generate diff image
   
4. OnComparisonComplete()
   └── Pass/fail based on pixel difference threshold
```

### Key Classes

| Class | Purpose |
|-------|---------|
| `AScreenshotFunctionalTestBase` | Base for screenshot comparison tests |
| `AScreenshotFunctionalTest` | Standard screenshot test |
| `AFunctionalUIScreenshotTest` | UI-specific screenshot test |
| `FAutomationScreenshotOptions` | Tolerance & comparison settings |

### Usage

Place a `ScreenshotFunctionalTest` actor in a map. Configure:
- **Camera**: Set `ScreenshotCamera` to the view
- **Options**: Tolerance, comparison mode, ignore regions
- **Variants**: Can generate multiple screenshots per test

### Session Frontend Integration

The **Screen Comparison** tab in Session Frontend shows:
- Side-by-side baseline vs. actual
- Difference visualization
- Per-pixel tolerance overlay

---

## 9. Layer 7 — UI Automation Driver

**Source**: `Engine/Source/Developer/AutomationDriver/`

A Selenium-like framework for **programmatic UI testing** of Slate widgets.

### Architecture

```cpp
// Find a button by widget ID
TSharedPtr<IDriverElement> Button = Driver->FindElement(
    LocateBy::WidgetId("SaveButton")
);

// Interact with it  
Button->Click();

// Wait for a condition
WaitUntil::ElementExists(LocateBy::WidgetType("SNotificationItem"));
```

### Key Interfaces

| Interface | Purpose |
|-----------|---------|
| `IAutomationDriver` | Main driver — controls the application |
| `IDriverElement` | Single UI element to interact with |
| `IElementLocator` | Strategy for finding elements |
| `IApplicationElement` | The application being tested |

### Locator Strategies

| Strategy | Description |
|----------|-------------|
| `LocateBy::WidgetId` | Find by assigned widget ID |
| `LocateBy::WidgetType` | Find by Slate widget class |
| `LocateBy::WidgetPath` | Find by path in widget tree |

---

## 10. Layer 8 — Python Automation Tests

**Source**: `Engine/Plugins/Tests/PythonAutomationTest/`

Python tests enable **scripted automation** without C++ compilation cycles.
Useful for content validation, editor workflows, and rapid test iteration.

### TestRunner API

```python
from automation_test.unittest_utilities import TestRunner

runner = TestRunner()

@runner.add_test
def test_asset_exists():
    assert unreal.EditorAssetLibrary.does_asset_exist("/Game/MyAsset")

@runner.add_test  
def test_spawn_actor():
    world = unreal.EditorLevelLibrary.get_editor_world()
    actor = unreal.EditorLevelLibrary.spawn_actor_from_class(
        unreal.StaticMeshActor, unreal.Vector(0, 0, 0))
    assert actor is not None

runner.set_before_each(lambda: print("Setup"))
runner.set_after_each(lambda: print("Teardown"))
runner.run_all()
```

### AutomationScheduler

For tests that need to span multiple editor ticks:

```python
from unreal_pythonautomationtest import AutomationScheduler

scheduler = AutomationScheduler()

def my_generator_test():
    # Frame 1: setup
    actor = spawn_actor()
    yield  # Wait one frame
    
    # Frame 2: validate
    assert actor.get_actor_location() != unreal.Vector(0, 0, 0)
    yield

scheduler.schedule(my_generator_test)
```

---

## 11. Layer 9 — Gauntlet (Distributed CI/CD Testing)

**Source**: `Engine/Source/Programs/AutomationTool/Gauntlet/`  
**Plugin**: `Engine/Plugins/Experimental/Gauntlet/`

Gauntlet is UE's **distributed test orchestration framework**. Written in C#, it
manages test execution across multiple machines, platforms, and device types.

### Architecture

```
┌──────────────────────────-─────────────────┐
│              Gauntlet Framework            │
├───────────-─┬──────────────┬───────────────┤
│ TestExecutor│  TestGroup   │  Platform     │
│ (scheduler) │  (grouping)  │  (Win/Mac/    │
│             │              │   Linux/iOS/  │
│             │              │   Android)    │
├────────────-┴──────────────┴───────────────┤
│              RunUnreal<T>                  │
│    (Base class for all UE test nodes)      │
├─────────────-──────────────────────────────┤
│  UnrealTestNode   │  EngineTestBase        │
│  (Game tests)     │  (Engine tests)        │
├──────────────────-┴────────────────────────┤
│         Platform Device Managers           │
│  (Win64Target, MacTarget, AndroidTarget)   │
└───────────────────-────────────────────────┘
```

### Key Components

| Component | File | Purpose |
|-----------|------|---------|
| `TestExecutor` | `Framework/Gauntlet.TestExecutor.cs` | Multi-threaded test scheduling |
| `RunUnreal<T>` | `Unreal/RunUnreal.cs` | Base UE test runner |
| `UnrealTestNode<T>` | `Unreal/Base/` | Game test node |
| `UnrealLogParser` | `Unreal/Utils/Gauntlet.UnrealLogParser.cs` | Parse UE log output |
| `AutomationLogParser` | `Unreal/Utils/Gauntlet.AutomationLogParser.cs` | Parse automation results |
| `LowLevelTestNode` | `LowLevelTests/Tests/` | Catch2 test integration |

### Capabilities

- **Multi-platform**: Windows, Mac, Linux, iOS, Android
- **Parallel execution**: Multiple tests across device farm
- **Log parsing**: Structured extraction of test results from UE logs  
- **Crash detection**: Minidump capture, stack trace analysis
- **Telemetry**: Performance metrics collection and regression detection
- **CI integration**: Exit codes, JSON/XML reports for build systems

### Running Gauntlet

```bash
# Run automation tests via Gauntlet
RunUAT.bat RunUnreal -test=DefaultTest -build=<path> -platform=Win64

# Run editor boot test
RunUAT.bat RunUnreal -test=EditorBootTest -project=MyGame
```

### Horde Integration

UE's **Horde** build system (successor to BuildGraph for CI) integrates with Gauntlet for:
- Automated test scheduling on build farm
- Test result aggregation across platforms
- Performance regression tracking
- Build health dashboards

Location: `Engine/Source/Programs/AutomationTool/Horde/`

---

## 12. Editor Test Runner UI

**Module**: `Engine/Source/Developer/AutomationWindow/`  
**Access**: **Windows → Developer Tools → Session Frontend → Automation tab**

### UI Layout

```
┌─────────────────────────────────────────────────────────┐
│ [▶ Run] [■ Stop] [↻ Refresh] [Presets ▼] [Export ▼]    │  ← Toolbar
├─────────────────────────────────────────────────────────┤
│ Filter: [___________________] [Smoke][Engine][Product]  │  ← Filters
├─────────────────────────┬───────────────────────────────┤
│ Test Tree               │ Test Output Log               │
│ ├─ System               │                               │
│ │  ├─ Core              │ [12:34:56] Running test...    │
│ │  │  ├─ Math ✓        │ [12:34:57] TestEqual passed   │
│ │  │  ├─ String ✓      │ [12:34:57] Test complete      │
│ │  │  └─ Container ✗   │                               │
│ │  └─ Rendering         │                               │
│ ├─ Project              │                               │
│ │  ├─ Combat ✓         │                               │
│ │  └─ Inventory ✗      │                               │
│ └─ Editor               │                               │
│    └─ Tools ✓           │                               │
├─────────────────────────┴───────────────────────────────┤
│ Pass: 47  Fail: 3  Skip: 2  Duration: 12.4s            │  ← Status bar
└─────────────────────────────────────────────────────────┘
```

### Features

- **Hierarchical test tree**: Tests organized by their dot-separated paths
- **Multi-device support**: Shows results per platform/device
- **Real-time output**: Log messages stream during execution
- **Tag filtering**: Filter by custom tags with search expressions
- **Test presets**: Save/load named test selections (stored in `Config/Automation/Presets/`)
- **Export**: Results to JSON/CSV
- **Screenshot comparison**: Dedicated tab for visual diff review

### Key Widget Classes

| Widget | Purpose |
|--------|---------|
| `SAutomationWindow` | Main test runner panel |
| `SAutomationWindowCommandBar` | Toolbar (run, stop, filter) |
| `SAutomationTestItem` | Individual test row in tree |
| `SAutomationGraphicalResultBox` | Visual result summary |
| `SAutomationExportMenu` | Export test results |
| `SAutomationTestTagFilter` | Tag-based filtering |

---

## 13. Command-Line Test Execution

### Running from Command Line

```bash
# Run specific tests
UnrealEditor.exe MyProject.uproject \
    -ExecCmds="Automation RunTests System.Core.Math" \
    -AutoQuit -Unattended -NullRHI

# Run all smoke tests
UnrealEditor.exe MyProject.uproject \
    -ExecCmds="Automation RunFilter Smoke" \
    -AutoQuit

# List all available tests
UnrealEditor.exe MyProject.uproject \
    -ExecCmds="Automation List" \
    -AutoQuit

# Run all tests 3 times with analytics
UnrealEditor.exe MyProject.uproject \
    -ExecCmds="Automation RunAll" \
    -TestLoops=3 -SendAutomationAnalytics -AutoQuit
```

### Available Commands

| Command | Description |
|---------|-------------|
| `Automation RunTests <filter>` | Run tests matching filter string |
| `Automation RunAll` | Run all tests |
| `Automation RunFilter <type>` | Run by category (Smoke, Engine, etc.) |
| `Automation List` | List all registered tests |
| `Automation DumpAll` | Dump detailed test info |

### Filter Types

| Filter | Tests Included |
|--------|---------------|
| `Smoke` | Quick sanity checks (seconds) |
| `Engine` | Engine subsystem tests |
| `Product` | Product/feature tests |
| `Perf` | Performance benchmarks |
| `Stress` | Long-running stress tests |
| `Negative` | Expected-failure tests |
| `Standard` | Smoke + Engine + Product + Perf |

### Useful Flags

| Flag | Effect |
|------|--------|
| `-AutoQuit` | Exit after tests complete |
| `-Unattended` | No user prompts |
| `-NullRHI` | No rendering (faster for non-visual tests) |
| `-TestLoops=N` | Repeat each test N times |
| `-FullSizeScreenshots` | Full-res screenshot capture |
| `-SendAutomationAnalytics` | Send telemetry data |

### Automation Execution States

```
Idle → Initializing → FindWorkers → RequestTests → DoingRequestedWork → Complete
```

---

## 14. Test Taxonomy — Flags, Priorities & Filters

Every test must declare its **flags** — a bitmask that determines *where* it runs,
*how fast* it is, and *how important* it is.

### Application Context (exactly one required)

| Flag | Meaning |
|------|---------|
| `EditorContext` | Requires the editor (most common) |
| `ClientContext` | Runs in a game client |
| `ServerContext` | Runs in a dedicated server |
| `CommandletContext` | Runs in a commandlet |
| `ProgramContext` | Runs in a standalone program |

### Speed Filter (exactly one required)

| Flag | Meaning | Typical Duration |
|------|---------|-----------------|
| `SmokeFilter` | Super fast sanity checks | < 5 seconds |
| `EngineFilter` | Engine subsystem tests | < 60 seconds |
| `ProductFilter` | Product/feature tests | < 5 minutes |
| `PerfFilter` | Performance benchmarks | Variable |
| `StressFilter` | Load/endurance tests | Long-running |
| `NegativeFilter` | Tests expecting failure | Variable |

### Priority (optional, one recommended)

| Flag | Meaning |
|------|---------|
| `CriticalPriority` | Showstopper — blocks release |
| `HighPriority` | Major feature functionality |
| `MediumPriority` | Minor feature functionality |
| `LowPriority` | Minor content bugs |

### Feature Flags (optional)

| Flag | Meaning |
|------|---------|
| `NonNullRHI` | Requires real rendering hardware |
| `RequiresUser` | Needs manual user intervention |
| `Disabled` | Skip this test (fast disable) |
| `SupportsAutoRTFM` | Can run inside AutoRTFM transactions |

### Example

```cpp
EAutomationTestFlags::EditorContext     // Runs in editor
    | EAutomationTestFlags::EngineFilter   // Engine-level speed
    | EAutomationTestFlags::HighPriority   // Important
    | EAutomationTestFlags::NonNullRHI     // Needs rendering
```

---

## 15. Latent Commands — Multi-Frame Test Execution

Game engines are **frame-based** — many operations (loading maps, waiting for physics,
streaming textures) can't complete in a single function call. UE solves this with
**latent commands**: deferred actions queued during `RunTest()` and executed across
subsequent frames.

### How It Works

```
Frame 1:  RunTest() → enqueues LatentCommand A, B, C
Frame 2:  A.Update() returns false (not done)
Frame 3:  A.Update() returns true (done!) → B starts
Frame 4:  B.Update() returns true → C starts
Frame 5:  C.Update() returns true → Test complete
```

### Defining a Latent Command

```cpp
// Simple latent command — no parameters
DEFINE_LATENT_AUTOMATION_COMMAND(FWaitForPhysicsSettle)
bool FWaitForPhysicsSettle::Update()
{
    static int32 FrameCount = 0;
    return ++FrameCount >= 60;  // Wait 60 frames, then done
}

// Latent command with parameters
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
    FLoadMapCommand, FString, MapPath)
bool FLoadMapCommand::Update()
{
    GEngine->Exec(nullptr, *FString::Printf(TEXT("open %s"), *MapPath));
    return true;  // Done immediately, but map loads asynchronously
}
```

### Using in Tests

```cpp
bool FMyTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FLoadMapCommand("/Game/Maps/TestMap"));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitForMapLoad());
    ADD_LATENT_AUTOMATION_COMMAND(FSpawnEnemies(10));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitForPhysicsSettle());
    ADD_LATENT_AUTOMATION_COMMAND(FValidateEnemyPositions());
    
    return true;  // Latent commands will execute across subsequent frames
}
```

### Built-In Latent Commands

| Command | Purpose |
|---------|---------|
| `FDelayForFramesLatentCommand` | Wait N frames |
| `FTakeScreenshotAfterTimeLatentCommand` | Screenshot after delay |
| `FThreadedAutomationLatentCommand` | Run work on background thread |
| `IAutomationLatentCommandWithRetriesAndDelays` | Retry with backoff |

---

## 16. Testing Plugins & Modules Map

### Core Testing Plugins (`Engine/Plugins/Tests/`)

| Plugin | Purpose |
|--------|---------|
| `RuntimeTests` | Runtime automation tests (editor + cooked) |
| `RHITests` | Rendering Hardware Interface tests |
| `CQTest` | Modern fixture-based test framework |
| `CQTestExperimental` | Experimental CQTest features |
| `CQTestEnhancedInputTests` | Enhanced Input System tests |
| `AudioCQTests` | Audio system tests |
| `TestFramework` | Base test framework utilities |
| `TestSamples` | Example/sample tests |
| `FunctionalTestingEditor` | Blueprint functional test editor tools |
| `PythonAutomationTest` | Python-based test automation |
| `InterchangeTests` | Asset interchange format tests |
| `AutomationDriverTests` | UI automation driver tests |
| `FbxAutomationTestBuilder` | FBX import/export tests |
| `WidgetAutomationTests` | Slate widget tests |

### Experimental Testing Plugins

| Plugin | Purpose |
|--------|---------|
| `Gauntlet` | Distributed CI/CD test framework |
| `AutomationUtils` | Automation utility helpers |
| `AsyncMessageSystemTests` | Async messaging tests |

### Domain-Specific Test Suites

| Suite | What It Tests |
|-------|---------------|
| `AITestSuite` | AI systems |
| `MassEntityTestSuite` | Mass Entity (ECS-like) framework |
| `MassAITestSuite` | Mass AI behaviors |
| `StructUtilsTestSuite` | Struct utilities |
| `MLAdapterTestSuite` | ML Adapter integration |
| `HTNTestSuite` | Hierarchical Task Network |
| `RigLogicLibTest` | Rig Logic animation |

### Developer Modules (`Engine/Source/Developer/`)

| Module | Purpose |
|--------|---------|
| `AutomationController` | Test management, discovery, execution |
| `AutomationDriver` | Programmatic UI testing |
| `AutomationWindow` | Editor test runner UI |
| `FunctionalTesting` | Map-based integration tests |
| `LowLevelTestsRunner` | Catch2 test executor |
| `CQTest` | CQTest core framework |

---

## 17. Key Source File Reference

| File | Lines | Purpose |
|------|-------|---------|
| `Engine/Source/Runtime/Core/Public/Misc/AutomationTest.h` | ~4600 | Core framework, macros, base classes |
| `Engine/Source/Runtime/Core/Private/Misc/AutomationTest.cpp` | ~2000 | Framework implementation |
| `Engine/Source/Runtime/Core/Public/Misc/AutomationEvent.h` | ~200 | Event/logging system |
| `Engine/Source/Developer/CQTest/Public/CQTest.h` | ~300 | CQTest macros |
| `Engine/Source/Developer/CQTest/README.md` | — | CQTest documentation |
| `Engine/Source/Developer/LowLevelTestsRunner/README.md` | — | Low-level test docs |
| `Engine/Source/Developer/FunctionalTesting/Classes/FunctionalTest.h` | ~500 | Actor-based tests |
| `Engine/Source/Developer/FunctionalTesting/Classes/ScreenshotFunctionalTest.h` | ~200 | Visual regression |
| `Engine/Source/Developer/AutomationController/Private/AutomationCommandline.cpp` | ~300 | CLI interface |
| `Engine/Source/Developer/AutomationWindow/Private/SAutomationWindow.h` | ~400 | Editor UI |
| `Engine/Source/Programs/AutomationTool/Gauntlet/Unreal/RunUnreal.cs` | ~400 | Gauntlet runner |

---

## 18. Architecture Diagram

```
┌──────────────────────────────────────────────────────────────────────────┐
│                           USER INTERFACES                                │
│  ┌─────────────────┐  ┌──────────────────┐  ┌────────────────────────┐  │
│  │  Editor UI      │  │  Command Line    │  │  Gauntlet CI/CD       │  │
│  │  (Automation    │  │  (-ExecCmds)     │  │  (RunUnreal, Horde)   │  │
│  │   Window)       │  │                  │  │                        │  │
│  └────────┬────────┘  └────────┬─────────┘  └───────────┬────────────┘  │
│           │                    │                         │               │
│           └────────────────────┼─────────────────────────┘               │
│                                ▼                                         │
│  ┌──────────────────────────────────────────────────────────────────┐    │
│  │                  AutomationController                            │    │
│  │    (Discovery, Scheduling, Execution, Reporting)                 │    │
│  └───────────────────────────┬──────────────────────────────────────┘    │
│                              ▼                                           │
│  ┌──────────────────────────────────────────────────────────────────┐    │
│  │              FAutomationTestFramework (Singleton)                 │    │
│  │                                                                  │    │
│  │  ┌──────────────────────────────────────────────────────────┐    │    │
│  │  │                    Test Registry                         │    │    │
│  │  │                                                          │    │    │
│  │  │  ┌──────────────┐  ┌──────────┐  ┌──────────────────┐  │    │    │
│  │  │  │ Simple Tests │  │ Complex  │  │ Network Tests    │  │    │    │
│  │  │  │ (RunTest)    │  │ Tests    │  │ (Multi-worker)   │  │    │    │
│  │  │  └──────────────┘  └──────────┘  └──────────────────┘  │    │    │
│  │  │  ┌──────────────┐  ┌──────────┐  ┌──────────────────┐  │    │    │
│  │  │  │ BDD Specs    │  │ CQTests  │  │ Functional Tests │  │    │    │
│  │  │  │ (Define)     │  │ (TEST)   │  │ (Actors in maps) │  │    │    │
│  │  │  └──────────────┘  └──────────┘  └──────────────────┘  │    │    │
│  │  └──────────────────────────────────────────────────────────┘    │    │
│  │                                                                  │    │
│  │  ┌─────────────────┐  ┌────────────────┐  ┌─────────────────┐   │    │
│  │  │ Latent Commands │  │ Expected Msgs  │  │ Telemetry Data  │   │    │
│  │  │ Queue           │  │ Validation     │  │ Collection      │   │    │
│  │  └─────────────────┘  └────────────────┘  └─────────────────┘   │    │
│  └──────────────────────────────────────────────────────────────────┘    │
│                                                                          │
│  OUT-OF-PROCESS:                                                         │
│  ┌──────────────────────────────────────────────────────────────────┐    │
│  │  Low-Level Tests (Catch2)  — Standalone executables, no editor   │    │
│  └──────────────────────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────────────────────┘
```

---

## 19. Lessons for Custom Engine Developers

Based on how UE5 structures its testing infrastructure, here are actionable patterns
for your own engine:

### 1. Registration-Based Discovery

UE uses **global static instances** created by macros to self-register tests at
startup. No separate test list to maintain — tests declare themselves.

**Takeaway**: Use a macro that expands to a global registrar object. Your test framework
singleton collects all tests automatically.

### 2. Hierarchical Naming

All tests use dot-separated paths (`System.Core.Math.Vector`). This gives you:
- Natural grouping in UIs
- Prefix-based filtering (`RunTests System.Core.*`)
- Organized test result reports

### 3. Flag Taxonomy

The flags system is critical: every test declares *where* it runs, *how fast* it is,
and *how important* it is. This lets CI pick the right subset per build type.

**Takeaway**: Design a bitflag system early. At minimum: context (editor/runtime),
speed tier, and priority.

### 4. Latent Commands for Frame-Based Systems

Single-frame unit tests aren't enough for engines. You need a command queue that
executes deferred actions across frames.

**Takeaway**: Implement an `ILatentCommand` interface with an `Update()` method called
each frame. Queue commands during `RunTest()`, execute them in your engine loop.

### 5. Multiple Framework Tiers

UE doesn't try to make one framework do everything:
- **Catch2** for fast, isolated C++ tests
- **Automation Tests** for engine-integrated tests  
- **CQTest** for modern fixture patterns
- **Specs** for BDD behavior descriptions
- **Functional Tests** for map-based integration tests
- **Screenshot Tests** for visual regression

**Takeaway**: Start with a lightweight unit test framework (Catch2/doctest), then add
an engine-integrated layer with latent command support. Add visual regression later.

### 6. Blueprint/Script Testing

Making tests accessible to non-programmers (via Blueprint or scripting) dramatically
increases test coverage. UE does this with `AFunctionalTest` as a Blueprintable actor.

**Takeaway**: Expose a test actor class to your scripting/visual scripting system.
Let designers place test actors in maps and override virtual events.

### 7. Screenshot Comparison Pipeline

Visual regression testing catches rendering bugs that unit tests can't. The key
challenges: determinism (disable temporal effects) and tolerance (pixel difference
thresholds).

### 8. CI/CD Integration

Gauntlet shows the value of a dedicated test orchestration layer that can:
- Parse engine log output for test results
- Coordinate across platforms and devices
- Generate reports in CI-consumable formats (JSON, XML, JUnit)

**Takeaway**: Design your test output format early so CI tools can parse it. Structured
log events (not just stdout) make automation much easier.

---

*Generated from analysis of UE 5.7 source code at `D:\Games\UE_5.7\Engine\Source\`*
