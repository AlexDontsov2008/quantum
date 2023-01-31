/*
** Copyright 2022 Bloomberg Finance L.P.
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include <cstdlib>
#include <stdexcept>
#include <functional>

#include <gtest/gtest.h>

#include <quantum/quantum.h>
#include <quantum_fixture.h>


namespace {
template <typename RET>
using CoroutineFunction = std::function<int(Bloomberg::quantum::CoroContextPtr<RET>)>;

struct TaskParams
{
    size_t contextSwitchCount{0}; // A number of times the task is supposed to switch the context. Coroutines only support.
    bool randomContextSwitchCount{false}; // If true the task will switch the context a random number of times in range [0, contextSwitchCount]. Coroutines only support.
    ms sleepTime{30}; // Sleep time between context switch. If contextSwitchCount is 0, the task doesn't sleep. For coroutine real work will take sleepTime / 2
    bool randomSleepTime{false}; // If true the task will sleep random time in range [0, sleepTime]
    bool throwException{false}; // Indicates if a task should throw exception
    size_t exceptionIteration{0}; // Iteration on which exception is thrown. Coroutines only support.
    Bloomberg::quantum::ITask::RetCode returnCode{Bloomberg::quantum::ITask::RetCode::Success}; // Return code of the task
};

template <typename RET = int>
CoroutineFunction<RET> makeCoroutineTask(const TaskParams& taskParams)
{
    return [taskParams](Bloomberg::quantum::CoroContextPtr<RET> ctx)-> int {
        size_t contextSwitchCount = taskParams.contextSwitchCount;
        if (taskParams.randomContextSwitchCount)
        {
            contextSwitchCount = rand() % (taskParams.contextSwitchCount + 1);
        }

        for (size_t iteration = 0; iteration < contextSwitchCount; ++iteration)
        {
            ms sleepTime = taskParams.sleepTime;
            if (taskParams.randomSleepTime && (sleepTime.count() > 0))
            {
                sleepTime = static_cast<ms>(rand() % sleepTime.count() + 1);
            }
            // IO operation time
            ctx->sleep(sleepTime);
            // CPU operation time
            std::this_thread::sleep_for(static_cast<ms>(sleepTime.count() / 2));
            if (taskParams.throwException && (taskParams.exceptionIteration == iteration))
            {
                throw std::runtime_error("Unexpected error");
            }
        }

        return static_cast<int>(taskParams.returnCode);
    };
}

template <typename RET = int>
std::function<int(Bloomberg::quantum::ThreadPromisePtr<RET>)>
makeIoTask(const TaskParams& taskParams)
{
    return [taskParams](Bloomberg::quantum::ThreadPromisePtr<RET>)-> int {
        ms sleepTime = taskParams.sleepTime;
        if (taskParams.randomSleepTime)
        {
            sleepTime = static_cast<ms>(rand() % taskParams.sleepTime.count() + 1);
        }
        std::this_thread::sleep_for(sleepTime);
        if (taskParams.throwException)
        {
            throw std::runtime_error("Unexpected error");
        }
        return static_cast<int>(taskParams.returnCode);
    };
}

template <typename BitField>
BitField unify(BitField lhs, BitField rhs)
{
    return static_cast<BitField>(static_cast<int>(lhs) | static_cast<int>(rhs));
}

// struct TaskStatesCounter {
//     explicit TaskStatesCounter(int initialized = 0,
//                                int started = 0,
//                                int resumed = 0,
//                                int suspended = 0,
//                                int stopped = 0)
//         : _initialized(initialized)
//         , _started(started)
//         , _resumed(resumed)
//         , _suspended(suspended)
//         , _stopped(stopped)
//     {}
//     void operator()(Bloomberg::quantum::TaskState state)
//     {
//         switch (state)
//         {
//             case Bloomberg::quantum::TaskState::Initialized:
//                 ++_initialized;
//                 break;
//             case Bloomberg::quantum::TaskState::Started:
//                 ++_started;
//                 break;
//             case Bloomberg::quantum::TaskState::Resumed:
//                 ++_resumed;
//                 break;
//             case Bloomberg::quantum::TaskState::Suspended:
//                 ++_suspended;
//                 break;
//             case Bloomberg::quantum::TaskState::Stopped:
//                 ++_stopped;
//                 break;
//             default:
//                 break;
//         }
//     }

//     void clear() {
//         _initialized = _started = _resumed = _suspended = _stopped = 0;
//     }

//     friend std::ostream& operator<<(std::ostream& out, const TaskStatesCounter& taskStatesCounter)
//     {
//         out << "initialized: " << taskStatesCounter._initialized << '\n'
//             << "started: " << taskStatesCounter._started << '\n'
//             << "resumed: " << taskStatesCounter._resumed << '\n'
//             << "suspended: " << taskStatesCounter._suspended << '\n'
//             << "stopped: " << taskStatesCounter._stopped;
//         return out;
//     }

//     std::atomic_int _initialized;
//     std::atomic_int _started;
//     std::atomic_int _resumed;
//     std::atomic_int _suspended;
//     std::atomic_int _stopped;
// };

class DispatcherTaskStateHandlerTest :
    public ::testing::TestWithParam<std::tuple<Bloomberg::quantum::TaskType,
                                               Bloomberg::quantum::TaskStateHandler>>
{
protected:
    DispatcherTaskStateHandlerTest()
    {
        srand((unsigned) time(NULL));
    }

    void SetUp() override {
        _handledTaskType = std::get<0>(GetParam());
        _taskStateHandler = std::get<1>(GetParam());
    }

    void testTaskStateHandler(
        size_t tasksCount,
        const TaskParams& taskParams,
        bool coroutineSharingForAny = false,
        bool loadBalanceSharedIoQueues = false,
        Bloomberg::quantum::TaskState handledTaskStates = Bloomberg::quantum::TaskState::All,
        Bloomberg::quantum::TaskStateHandler taskStateHandler = {},
        CoroutineFunction<int> userCoroutineTask = {})
    {
        TaskStatesCounter taskStatesCounter;
        if (taskStateHandler)
        {
            _taskStateHandler = taskStateHandler;
        }

        if (_taskStateHandler)
        {
            taskStateHandler = [this, &taskStatesCounter]
            (size_t taskId, int queueId, Bloomberg::quantum::TaskType type, Bloomberg::quantum::TaskState state)
            {
                taskStatesCounter(state);
                _taskStateHandler(taskId, queueId, type, state);
            };
        }

        Bloomberg::quantum::TaskStateConfiguration taskStateConfiguration;
        taskStateConfiguration.setTaskStateHandler(taskStateHandler);
        taskStateConfiguration.setHandledTaskStates(handledTaskStates);
        taskStateConfiguration.setHandledTaskTypes(_handledTaskType);
        const TestConfiguration config(loadBalanceSharedIoQueues,
                                       coroutineSharingForAny,
                                       taskStateConfiguration);
        auto dispatcher = DispatcherSingleton::createInstance(config);

        CoroutineFunction<int> coroutineTask = makeCoroutineTask(taskParams);
        if (userCoroutineTask)
        {
            coroutineTask = userCoroutineTask;
        }
        for(size_t taskId = 0; taskId < tasksCount; ++taskId)
        {
            switch (_handledTaskType)
            {
                case Bloomberg::quantum::TaskType::None:
                    break;
                case Bloomberg::quantum::TaskType::Coroutine: {
                    dispatcher->post(coroutineTask);
                    break;
                }
                case Bloomberg::quantum::TaskType::IoTask: {
                    dispatcher->postAsyncIo(makeIoTask(taskParams));
                    break;
                }
                case Bloomberg::quantum::TaskType::All: {
                    dispatcher->post(coroutineTask);
                    dispatcher->postAsyncIo(makeIoTask(taskParams));
                    break;
                }
                default:
                    break;
            }
        }
        dispatcher->drain();

        std::cout << taskStatesCounter << std::endl;
        EXPECT_EQ(taskStatesCounter._initialized, 0);
        const bool isStartedStateHandled =
            Bloomberg::quantum::isIntersection(handledTaskStates, Bloomberg::quantum::TaskState::Started);
        if (isStartedStateHandled and not userCoroutineTask)
        {
            size_t expectedStartedCount = 0;
            switch (_handledTaskType)
            {
                case Bloomberg::quantum::TaskType::None: {
                    expectedStartedCount = 0;
                    break;
                }
                case Bloomberg::quantum::TaskType::Coroutine: {
                    expectedStartedCount = tasksCount;
                    break;
                }
                case Bloomberg::quantum::TaskType::IoTask: {
                    expectedStartedCount = tasksCount;
                    break;
                }
                case Bloomberg::quantum::TaskType::All: {
                    expectedStartedCount = 2 * tasksCount;
                    break;
                }
                default:
                    break;
            }
            EXPECT_EQ(taskStatesCounter._started, expectedStartedCount);
        }
        EXPECT_EQ(taskStatesCounter._started, taskStatesCounter._stopped);
        EXPECT_EQ(taskStatesCounter._resumed, taskStatesCounter._suspended);
        if (Bloomberg::quantum::TaskType::IoTask == _handledTaskType)
        {
            EXPECT_EQ(taskStatesCounter._resumed, 0);
        }
    }

    Bloomberg::quantum::TaskType _handledTaskType;
    Bloomberg::quantum::TaskStateHandler _taskStateHandler;
};

enum class SequencerType {
    None,
    Normal,
    Experimental
};

class SequencerTaskStateHandlerTest :
    public ::testing::TestWithParam<std::tuple<Bloomberg::quantum::TaskType, SequencerType>>
{
protected:
    SequencerTaskStateHandlerTest()
    : _sequencerType(SequencerType::None)
    {
        srand((unsigned) time(NULL));
    }

    void SetUp() override {
        _handledTaskType = std::get<0>(GetParam());
        _sequencerType = std::get<1>(GetParam());
    }

    void testTaskStateHandler(
        size_t tasksCount,
        const TaskParams& taskParams,
        bool coroutineSharingForAny = false,
        bool loadBalanceSharedIoQueues = false,
        CoroutineFunction<Bloomberg::quantum::Void> userCoroutineTask = {})
    {
        TaskStatesCounter taskStatesCounter;
        auto taskStateHandler = [&taskStatesCounter]
        (size_t, int, Bloomberg::quantum::TaskType taskType, Bloomberg::quantum::TaskState state)
        {
            EXPECT_EQ(taskType, Bloomberg::quantum::TaskType::Coroutine);
            taskStatesCounter(state);
        };

        Bloomberg::quantum::TaskStateConfiguration taskStateConfiguration;
        taskStateConfiguration.setTaskStateHandler(taskStateHandler);
        taskStateConfiguration.setHandledTaskStates(Bloomberg::quantum::TaskState::All);
        taskStateConfiguration.setHandledTaskTypes(_handledTaskType);
        const TestConfiguration config(loadBalanceSharedIoQueues,
                                       coroutineSharingForAny,
                                       taskStateConfiguration);
        auto dispatcher = DispatcherSingleton::createInstance(config);


        // Initialize sequencer
        std::function<bool(std::chrono::milliseconds, bool)> drain;
        switch (_sequencerType)
        {
            case SequencerType::Normal:
            {
                _normalSequencer =
                    std::make_unique<Bloomberg::quantum::Sequencer<SequencerKey>>(*dispatcher);
                _experimentalSequencer.reset();
                drain = [this](std::chrono::milliseconds timeout, bool isFinal) {
                    return _normalSequencer->drain(timeout, isFinal);
                };
                break;
            }
            case SequencerType::Experimental:
            {
                _experimentalSequencer =
                    std::make_unique<Bloomberg::quantum::experimental::Sequencer<SequencerKey>>(*dispatcher);
                _normalSequencer.reset();
                drain = [this](std::chrono::milliseconds timeout, bool isFinal) {
                    return _experimentalSequencer->drain(timeout, isFinal);
                };
                break;
            }
            default:
            {
                throw std::runtime_error("Undefined type of sequencer");
            }
        }

        auto task = makeCoroutineTask<Bloomberg::quantum::Void>(taskParams);
        if (userCoroutineTask)
        {
            task = userCoroutineTask;
        }
        const SequencerKey key(0);
        for (size_t taskId = 0; taskId < tasksCount; ++taskId)
        {
            switch (_sequencerType)
            {
                case SequencerType::Normal:
                {
                    _normalSequencer->enqueue(key, task);
                    break;
                }
                case SequencerType::Experimental:
                {
                    _experimentalSequencer->enqueue(key, task);
                    break;
                }
                default:
                {
                    throw std::runtime_error("Undefined type of sequencer");
                }
            }
        }
        drain(std::chrono::milliseconds(-1), false);

        std::cout << taskStatesCounter << std::endl;
        EXPECT_EQ(taskStatesCounter._initialized, 0);
        if (not userCoroutineTask)
        {
            EXPECT_EQ(taskStatesCounter._started, tasksCount);
        }
        EXPECT_EQ(taskStatesCounter._started, taskStatesCounter._stopped);
        if (not taskParams.randomContextSwitchCount)
        {
            EXPECT_EQ(taskParams.contextSwitchCount, taskStatesCounter._resumed);
        }
        EXPECT_EQ(taskStatesCounter._resumed, taskStatesCounter._suspended);

    }

    using SequencerKey = int;
    std::unique_ptr<Bloomberg::quantum::Sequencer<SequencerKey>> _normalSequencer;
    std::unique_ptr<Bloomberg::quantum::experimental::Sequencer<SequencerKey>> _experimentalSequencer;

    Bloomberg::quantum::TaskType _handledTaskType;
    SequencerType _sequencerType;
};

// Handlers
Bloomberg::quantum::TaskStateHandler EmptyHandler = [](size_t, int, Bloomberg::quantum::TaskType, Bloomberg::quantum::TaskState){};
Bloomberg::quantum::TaskStateHandler MemoryManagementHandler = TestTaskStateHandler();

// Task handled states
const auto StartedAndStoppedHandledStates = unify(Bloomberg::quantum::TaskState::Started,
                                                  Bloomberg::quantum::TaskState::Stopped);
const auto ResumedAndSuspendedHandledStates = unify(Bloomberg::quantum::TaskState::Resumed,
                                                    Bloomberg::quantum::TaskState::Suspended);

void testTaskStatesCounter(const TaskStatesCounter& expected, const TaskStatesCounter& actual)
{
    EXPECT_EQ(expected._initialized, actual._initialized);
    EXPECT_EQ(expected._started, actual._started);
    EXPECT_EQ(expected._resumed, actual._resumed);
    EXPECT_EQ(expected._suspended, actual._suspended);
    EXPECT_EQ(expected._stopped, actual._stopped);
}

void testHandleTaskType(Bloomberg::quantum::TaskType taskType,
                        size_t tasksCount,
                        const TaskParams& taskParams,
                        const TaskStatesCounter& expectedTaskStatesCounter,
                        bool loadBalance = false,
                        bool coroutineSharingForAny = false,
                        bool isHandlerAvailable = true)
{
    TaskStatesCounter actualTaskStatesCounter;
    Bloomberg::quantum::TaskStateConfiguration taskStateConfiguration;
    taskStateConfiguration.setTaskStateHandler(
        [&actualTaskStatesCounter] (size_t, int, Bloomberg::quantum::TaskType, Bloomberg::quantum::TaskState state)
        {
            actualTaskStatesCounter(state);
        });

    taskStateConfiguration.setHandledTaskStates(Bloomberg::quantum::TaskState::All);
    taskStateConfiguration.setHandledTaskTypes(taskType);

    if (not isHandlerAvailable)
    {
        taskStateConfiguration.setTaskStateHandler({});
    }

    auto dispatcher = DispatcherSingleton::createInstance(
        TestConfiguration(loadBalance, coroutineSharingForAny, taskStateConfiguration));
    for (size_t taskId = 0; taskId < tasksCount; ++taskId)
    {
        dispatcher->post(makeCoroutineTask(taskParams));
        dispatcher->postAsyncIo(makeIoTask(taskParams));
    }
    dispatcher->drain();

    std::cout << actualTaskStatesCounter << std::endl;
    testTaskStatesCounter(expectedTaskStatesCounter, actualTaskStatesCounter);
}

void testHandleTaskState(const std::vector<Bloomberg::quantum::TaskState>& states,
                         Bloomberg::quantum::TaskState handledStates,
                         const TaskStatesCounter& expectedTaskStatesCounter)
{
    TaskStatesCounter actualTaskStatesCounter;
    bool isHandlerCalled = false;
    auto handler = [&isHandlerCalled, &actualTaskStatesCounter]
        (size_t, int, Bloomberg::quantum::TaskType, Bloomberg::quantum::TaskState state)
    {
        actualTaskStatesCounter(state);
        isHandlerCalled = true;
    };

    auto state = Bloomberg::quantum::TaskState::Initialized;
    for (auto nextState : states)
    {
        isHandlerCalled = false;
        handleTaskState(handler,
                    0,
                    0,
                    Bloomberg::quantum::TaskType::None,
                    handledStates,
                    nextState,
                    state);
        EXPECT_TRUE(Bloomberg::quantum::isIntersection(handledStates, nextState) == isHandlerCalled);
        EXPECT_EQ(state, nextState);
    }

    testTaskStatesCounter(expectedTaskStatesCounter, actualTaskStatesCounter);
}


} // namespace

INSTANTIATE_TEST_CASE_P(DispatcherTaskStateHandlerTest_Default,
                         DispatcherTaskStateHandlerTest,
                         ::testing::Combine(
                            ::testing::Values(
                                Bloomberg::quantum::TaskType::Coroutine,
                                Bloomberg::quantum::TaskType::IoTask,
                                Bloomberg::quantum::TaskType::All
                            ),
                            ::testing::Values(
                                TestTaskStateHandler())));

INSTANTIATE_TEST_CASE_P(SequencerTaskStateHandlerTest_Default,
                         SequencerTaskStateHandlerTest,
                         ::testing::Combine(
                            ::testing::Values(
                                Bloomberg::quantum::TaskType::All),
                            ::testing::Values(
                                SequencerType::Normal,
                                SequencerType::Experimental)));

//==============================================================================
//                             HANDLE TASK STATE TEST CASES
//==============================================================================

TEST(TaskStateHandlerTest, UnableToHandleTaskState)
{
    TaskStatesCounter taskStatesCounter;
    bool isHandlerCalled = false;
    auto handler = [&isHandlerCalled, &taskStatesCounter]
    (size_t, int, Bloomberg::quantum::TaskType, Bloomberg::quantum::TaskState state)
    {
        taskStatesCounter(state);
        isHandlerCalled = true;
    };

    // Wrong task state order
    auto state = Bloomberg::quantum::TaskState::Stopped;
    handleTaskState(handler,
                    0,
                    0,
                    Bloomberg::quantum::TaskType::None,
                    Bloomberg::quantum::TaskState::None,
                    Bloomberg::quantum::TaskState::Started,
                    state);
    EXPECT_FALSE(isHandlerCalled);
    EXPECT_EQ(state, Bloomberg::quantum::TaskState::Stopped);

    // No states processed
    state = Bloomberg::quantum::TaskState::Initialized;
    handleTaskState(handler,
                    0,
                    0,
                    Bloomberg::quantum::TaskType::None,
                    Bloomberg::quantum::TaskState::None,
                    Bloomberg::quantum::TaskState::Started,
                    state);
    EXPECT_FALSE(isHandlerCalled);
    EXPECT_EQ(state, Bloomberg::quantum::TaskState::Started);

    // Missing handled state
    handleTaskState(handler,
                    0,
                    0,
                    Bloomberg::quantum::TaskType::None,
                    unify(Bloomberg::quantum::TaskState::Started,
                          Bloomberg::quantum::TaskState::Stopped),
                    Bloomberg::quantum::TaskState::Suspended,
                    state);
    EXPECT_FALSE(isHandlerCalled);
    EXPECT_EQ(state, Bloomberg::quantum::TaskState::Suspended);
}

TEST(TaskStateHandlerTest, HandleTaskState)
{
    // [Initialized -> Started -> [Suspended -> Resumed] x 2 -> Stopped]
    const std::vector<Bloomberg::quantum::TaskState> fullStatesSequence = {
        Bloomberg::quantum::TaskState::Started,
        Bloomberg::quantum::TaskState::Suspended,
        Bloomberg::quantum::TaskState::Resumed,
        Bloomberg::quantum::TaskState::Suspended,
        Bloomberg::quantum::TaskState::Resumed,
        Bloomberg::quantum::TaskState::Stopped
    };

    // [Initialized -> Started -> Stopped]
    const std::vector<Bloomberg::quantum::TaskState> startedAndStoppedSequence = {
        Bloomberg::quantum::TaskState::Started,
        Bloomberg::quantum::TaskState::Stopped
    };
    testHandleTaskState(fullStatesSequence,
                        Bloomberg::quantum::TaskState::All,
                        TaskStatesCounter(0, 1, 2, 2, 1));

    testHandleTaskState(startedAndStoppedSequence,
                        Bloomberg::quantum::TaskState::All,
                        TaskStatesCounter(0, 1, 0, 0, 1));

    testHandleTaskState(fullStatesSequence,
                        StartedAndStoppedHandledStates,
                        TaskStatesCounter(0, 1, 0, 0, 1));

    testHandleTaskState(startedAndStoppedSequence,
                        StartedAndStoppedHandledStates,
                        TaskStatesCounter(0, 1, 0, 0, 1));

    testHandleTaskState(fullStatesSequence,
                        ResumedAndSuspendedHandledStates,
                        TaskStatesCounter(0, 0, 2, 2, 0));

    testHandleTaskState(startedAndStoppedSequence,
                        ResumedAndSuspendedHandledStates,
                        TaskStatesCounter(0, 0, 0, 0, 0));

}

TEST(TaskStateHandlerTest, HandleDifferentTaskTypes)
{
    const TaskParams taskParams{1, false, ms(100), true};
    const size_t tasksCount = 100;

    ////////////////////////////
    // No task state handling
    ////////////////////////////

    testHandleTaskType(Bloomberg::quantum::TaskType::None,
                       tasksCount,
                       taskParams,
                       TaskStatesCounter(0, 0, 0, 0, 0));

    ////////////////////////////
    // Coroutine state handling
    ////////////////////////////

    // Without shared coroutine queue
    testHandleTaskType(Bloomberg::quantum::TaskType::Coroutine,
                       tasksCount,
                       taskParams,
                       TaskStatesCounter(0, tasksCount, tasksCount, tasksCount, tasksCount),
                       false,
                       false);

    // With shared coroutine queue
    testHandleTaskType(Bloomberg::quantum::TaskType::Coroutine,
                       tasksCount,
                       taskParams,
                       TaskStatesCounter(0, tasksCount, tasksCount, tasksCount, tasksCount),
                       false,
                       true);

    // Without handler
    testHandleTaskType(Bloomberg::quantum::TaskType::Coroutine,
                       tasksCount,
                       taskParams,
                       TaskStatesCounter(0, 0, 0, 0, 0),
                       false,
                       false,
                       false);

    ////////////////////////////
    // IoTask state handling
    ////////////////////////////

    // Without shared IO queue
    testHandleTaskType(Bloomberg::quantum::TaskType::IoTask,
                       tasksCount,
                       taskParams,
                       TaskStatesCounter(0, tasksCount, 0, 0, tasksCount),
                       false,
                       false);

    // With shared IO queue
    testHandleTaskType(Bloomberg::quantum::TaskType::IoTask,
                       tasksCount,
                       taskParams,
                       TaskStatesCounter(0, tasksCount, 0, 0, tasksCount),
                       true,
                       false);

    // Without handler
    testHandleTaskType(Bloomberg::quantum::TaskType::IoTask,
                       tasksCount,
                       taskParams,
                       TaskStatesCounter(0, 0, 0, 0, 0),
                       false,
                       false,
                       false);

    /////////////////////////////////
    // All task types state handling
    /////////////////////////////////

    // Without shared IO queue
    testHandleTaskType(Bloomberg::quantum::TaskType::All,
                       tasksCount,
                       taskParams,
                       TaskStatesCounter(0, tasksCount * 2, tasksCount, tasksCount, tasksCount * 2),
                       false,
                       false);

    // With shared IO queue and coroutine queue
    testHandleTaskType(Bloomberg::quantum::TaskType::All,
                       tasksCount,
                       taskParams,
                       TaskStatesCounter(0, tasksCount * 2, tasksCount, tasksCount, tasksCount * 2),
                       true,
                       true);

    // Without handler
    testHandleTaskType(Bloomberg::quantum::TaskType::All,
                       tasksCount,
                       taskParams,
                       TaskStatesCounter(0, 0, 0, 0, 0),
                       false,
                       false,
                       false);
}

//==============================================================================
//                            DISPATCHER TEST CASES
//==============================================================================

TEST_P(DispatcherTaskStateHandlerTest, NoContextSwitch)
{
    testTaskStateHandler(100, {0, false, ms(100), true});
}

TEST_P(DispatcherTaskStateHandlerTest, WithContextSwitch)
{
    testTaskStateHandler(100, {3, true, ms(100), true});
}

TEST_P(DispatcherTaskStateHandlerTest, NoContextSwitchOnSharedQueue)
{
    testTaskStateHandler(100, {0, false, ms(100), true}, true);
}

TEST_P(DispatcherTaskStateHandlerTest, WithContextSwitchOnSharedQueue)
{
    testTaskStateHandler(100, {3, true, ms(100), true}, true);
}

TEST_P(DispatcherTaskStateHandlerTest, NoContextSwitchWithLoadBalanceOnSharedIoQueues)
{
    testTaskStateHandler(100, {0, false, ms(100), true}, false, true);
}

TEST_P(DispatcherTaskStateHandlerTest, WithContextSwitchWithLoadBalanceOnSharedIoQueues)
{
    testTaskStateHandler(100, {3, true, ms(100), true}, false, true);
}

TEST_P(DispatcherTaskStateHandlerTest, NoContextSwitchTaskException)
{
    testTaskStateHandler(100, {0, false, ms(100), true, true});
}

TEST_P(DispatcherTaskStateHandlerTest, WithContextSwitchWithException)
{
    testTaskStateHandler(100, {2, false, ms(100), true, true, 1});
}

TEST_P(DispatcherTaskStateHandlerTest, NoContextSwitchWithTaskExceptionOnSharedQueue)
{
    testTaskStateHandler(100, {0, false, ms(100), true, true}, true);
}

TEST_P(DispatcherTaskStateHandlerTest, WithContextSwitchWithTaskExceptionOnSharedQueue)
{
    testTaskStateHandler(100, {2, false, ms(100), true, true, 1}, true);
}

TEST_P(DispatcherTaskStateHandlerTest, NoContextSwitchWithTaskCodeException)
{
    testTaskStateHandler(100, {0, false, ms(100), true, false, 0, Bloomberg::quantum::ITask::RetCode::Exception});
}

TEST_P(DispatcherTaskStateHandlerTest, WithContextSwitchWithTaskCodeException)
{
    testTaskStateHandler(100, {2, false, ms(100), true, false, 0, Bloomberg::quantum::ITask::RetCode::Exception});
}

TEST_P(DispatcherTaskStateHandlerTest, PostTaskFromWithinCoroutine)
{
    // Reset handled task type to check only for coroutines

    std::atomic_int outerTaskCallsCounter = 0;
    std::atomic_int innerTaskCallsCounter = 0;

    TaskParams taskParams;
    taskParams.contextSwitchCount = 2;
    taskParams.randomContextSwitchCount = true;
    auto switchTask = makeCoroutineTask(taskParams);
    auto innerTask = [&innerTaskCallsCounter, &switchTask] (Bloomberg::quantum::CoroContextPtr<int> ctx) -> int {
        ++innerTaskCallsCounter;
        switchTask(ctx);
        return 0;
    };
    auto task = [&outerTaskCallsCounter, &innerTask, &switchTask] (Bloomberg::quantum::CoroContextPtr<int> ctx) -> int {
        switchTask(ctx);
        ctx->post(innerTask);
        ++outerTaskCallsCounter;
        return 0;
    };
    testTaskStateHandler(100, {}, false, false, Bloomberg::quantum::TaskState::All, {}, task);
    EXPECT_EQ(innerTaskCallsCounter, outerTaskCallsCounter);
}

TEST_P(DispatcherTaskStateHandlerTest, HandleNoneTaskStates)
{
    testTaskStateHandler(100, {3, true, ms(100), true}, false, false, Bloomberg::quantum::TaskState::None);
}

TEST_P(DispatcherTaskStateHandlerTest, HandleStartedAndStoppedTaskStates)
{
    testTaskStateHandler(100, {3, true, ms(100), true}, false, false, StartedAndStoppedHandledStates);
}

TEST_P(DispatcherTaskStateHandlerTest, HandleResumedAndSuspendedTaskStates)
{
    // Use empty handler here to avoid issues with checks in the default handler
    testTaskStateHandler(100, {3, true, ms(100), true}, false, false, ResumedAndSuspendedHandledStates, EmptyHandler);
}

TEST_P(DispatcherTaskStateHandlerTest, HandleAllTaskStates)
{
    testTaskStateHandler(100, {3, true, ms(100), true}, false, false, Bloomberg::quantum::TaskState::All);
}

TEST_P(DispatcherTaskStateHandlerTest, LongRunningTask)
{
    testTaskStateHandler(20, {2, true, ms(1000), false, false, 0, Bloomberg::quantum::ITask::RetCode::Exception});
}

TEST_P(DispatcherTaskStateHandlerTest, StressTest)
{
    testTaskStateHandler(5000, {2, true, ms(50), true});
}

//==============================================================================
//                            SEQUENCER TEST CASES
//==============================================================================

TEST_P(SequencerTaskStateHandlerTest, NoContextSwitch)
{
    testTaskStateHandler(100, {0, false, ms(100), false});
}

TEST_P(SequencerTaskStateHandlerTest, WithContextSwitch)
{
    testTaskStateHandler(100, {2, false, ms(100), false});
}

TEST_P(SequencerTaskStateHandlerTest, NoContextSwitchOnSharedQueue)
{
    testTaskStateHandler(100, {0, false, ms(100), true}, true);
}

TEST_P(SequencerTaskStateHandlerTest, WithContextSwitchOnSharedQueue)
{
    testTaskStateHandler(100, {3, true, ms(100), true}, true);
}

TEST_P(SequencerTaskStateHandlerTest, PostTaskFromWithinCoroutine)
{
    // Reset handled task type to check only for coroutines

    std::atomic_int outerTaskCallsCounter = 0;
    std::atomic_int innerTaskCallsCounter = 0;

    TaskParams taskParams;
    taskParams.contextSwitchCount = 2;
    taskParams.randomContextSwitchCount = true;
    auto switchTask = makeCoroutineTask<Bloomberg::quantum::Void>(taskParams);
    auto innerTask = [&innerTaskCallsCounter, &switchTask] (Bloomberg::quantum::CoroContextPtr<Bloomberg::quantum::Void> ctx) -> int {
        ++innerTaskCallsCounter;
        switchTask(ctx);
        return 0;
    };
    auto task = [&outerTaskCallsCounter, &innerTask, &switchTask] (Bloomberg::quantum::CoroContextPtr<Bloomberg::quantum::Void> ctx) -> int {
        switchTask(ctx);
        ctx->post(innerTask);
        ++outerTaskCallsCounter;
        return 0;
    };
    testTaskStateHandler(100, {}, false, false, task);
    EXPECT_EQ(innerTaskCallsCounter, outerTaskCallsCounter);
}
