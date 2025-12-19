// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#pragma once

/**
 * @file Runnable.h
 * @brief Interface for "runnable" objects for worker threads
 *
 * Ported from Unreal Engine's HAL/Runnable.h
 */

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    // Forward declarations
    class FSingleThreadRunnable;

    /**
     * @class FRunnable
     * @brief Interface for objects that can be run on a thread
     *
     * This is the interface for code that can be "run" on a thread.
     * It provides hooks for initialization, the main work loop,
     * early termination requests, and cleanup.
     */
    class FRunnable
    {
      public:
        /**
         * @brief Initializes the runnable object
         *
         * Called in the context of the thread object that will run this
         * runnable, before Run() is called.
         *
         * @return True if initialization was successful, false to abort the thread
         */
        virtual bool Init()
        {
            return true;
        }

        /**
         * @brief Runs the runnable object
         *
         * This is where all the actual work is done. This is called after
         * Init() completes successfully, and should return when the runnable
         * is finished.
         *
         * @return The exit code for the thread
         */
        virtual u32 Run() = 0;

        /**
         * @brief Stops the runnable object
         *
         * Called by another thread to request early termination. The runnable
         * should check for stop requests periodically and exit Run() when
         * appropriate.
         */
        virtual void Stop()
        {
        }

        /**
         * @brief Exits the runnable object
         *
         * Called in the context of the thread object after Run() completes.
         * This is where cleanup should occur.
         */
        virtual void Exit()
        {
        }

        /**
         * @brief Gets the single-threaded interface
         *
         * If the platform doesn't support multithreading, this interface
         * can be used to tick the runnable from the main thread instead.
         *
         * @return Pointer to single-thread interface, or nullptr if not supported
         */
        virtual FSingleThreadRunnable* GetSingleThreadInterface()
        {
            return nullptr;
        }

        /**
         * @brief Virtual destructor
         */
        virtual ~FRunnable() = default;
    };

    /**
     * @class FSingleThreadRunnable
     * @brief Interface for runnables that support single-threaded mode
     *
     * When multithreading is not available (e.g., some mobile platforms
     * or when -nothreading is specified), this interface allows the
     * runnable's work to be ticked from the main thread.
     */
    class FSingleThreadRunnable
    {
      public:
        /**
         * @brief Ticks the runnable
         *
         * Called repeatedly from the main thread when actual threading
         * is not available.
         */
        virtual void Tick() = 0;

        /**
         * @brief Virtual destructor
         */
        virtual ~FSingleThreadRunnable() = default;
    };

} // namespace OloEngine
