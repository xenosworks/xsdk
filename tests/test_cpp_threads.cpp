#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "tests.h"
#include "xsdk/thread.h"

namespace
{

thread_local int thread_tag = -1;

struct Rally {
	std::mutex mutex;
	std::condition_variable turn_changed;
	int turn = 0;
	int hits = 0;
};

// Two players take turns, so every hit hands the rally over through the mutex and condition variable.
void play(Rally &rally, int player, int rounds, int &tag_seen)
{
	thread_tag = player;
	for (int i = 0; i < rounds; i++) {
		std::unique_lock lock(rally.mutex);
		rally.turn_changed.wait(lock, [&] { return rally.turn == player; });
		rally.hits++;
		rally.turn = 1 - player;
		rally.turn_changed.notify_all();
	}
	tag_seen = thread_tag;
}

} // namespace

extern "C" void test_cpp_threads(void)
{
	constexpr int rounds = 100;
	Rally rally;
	int tags[2] = { -1, -1 };
	thread_tag = 7;
	{
		const std::jthread first(play, std::ref(rally), 0, rounds, std::ref(tags[0]));
		const std::jthread second(play, std::ref(rally), 1, rounds, std::ref(tags[1]));
	}
	check("jthreads share a mutex and condition variable", rally.hits == 2 * rounds);
	check("thread_local differs per thread", tags[0] == 0 && tags[1] == 1 && thread_tag == 7);
	HANDLE gate = 0;
	if (!NT_SUCCESS(NtCreateEvent(&gate, nullptr, NOTIFICATION_EVENT, 0))) {
		check("thread control: create C++ gate", false);
		return;
	}
	uint32_t observed = UINT32_MAX;
	NTSTATUS waited = STATUS_TIMEOUT;
	std::thread worker([&] {
		const int64_t timeout = -10000000;
		waited = NtWaitForSingleObjectEx(gate, USER_MODE, 0, &timeout);
		observed = xsdk_thread_hardware_thread();
	});
	const HANDLE handle = xsdk_pthread_handle(worker.native_handle());
	const NTSTATUS placed = xsdk_thread_place(handle, 5);
	const NTSTATUS raised = xsdk_thread_set_priority(handle, 1, nullptr);
	const NTSTATUS released = NtSetEvent(gate, nullptr);
	worker.join();
	check("thread control: a std::thread moves through its borrowed kernel handle",
		NT_SUCCESS(placed) && NT_SUCCESS(raised) && NT_SUCCESS(released) && waited == STATUS_SUCCESS && observed == 5);
	(void)NtClose(gate);
}
