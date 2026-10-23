#include "listener.hpp"

#include <glib-object.h>
#include <glib.h>
#include <qlist.h>
#include <qtclasshelpermacros.h>
#include <qtest.h>
#include <qtestcase.h>
#include <qtmetamacros.h>

#define POLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE
// Workaround macro collision with glib 'signals' struct member.
#undef signals
#include <gio/gio.h>
#include <polkit/polkit.h>
#include <polkitagent/polkitagent.h>
#define signals Q_SIGNALS

#include "../listener.hpp"

using qs::service::polkit::AuthRequest;
using qs::service::polkit::ListenerCb;

namespace {

// Stands in for PolkitAgentImpl. It records the requests the listener hands
// it and only compares their addresses, since a call for a request that has
// already been deleted is exactly what these tests look for.
class FakeAgent: public ListenerCb {
public:
	void registerComplete(bool /*success*/) override {}

	void initiateAuthentication(AuthRequest* request) override { this->started.append(request); }

	void cancelAuthentication(AuthRequest* request) override {
		this->cancelled.append(request);

		// PolkitAgentImpl deletes a queued request from inside this call.
		if (this->deleteOnCancel) {
			request->cancel("Authentication request was cancelled");
			delete request;
		}
	}

	QList<AuthRequest*> started;
	QList<AuthRequest*> cancelled;
	bool deleteOnCancel = false;
};

// Stands in for libpolkit-agent's side of a request (polkitagentlistener.c):
// a fresh cancellable per request, dropped when the listener answers, and a
// CancelAuthentication from polkitd that only reaches a request still
// waiting for its answer.
class FakePolkit {
public:
	explicit FakePolkit(QsPolkitAgent* listener): listener(listener) {}
	~FakePolkit() { g_clear_object(&this->cancellable); }
	Q_DISABLE_COPY_MOVE(FakePolkit);

	void begin() {
		auto* identities = g_list_append(nullptr, polkit_unix_user_new(0));
		auto* details = polkit_details_new();

		polkit_agent_listener_initiate_authentication(
		    POLKIT_AGENT_LISTENER(this->listener),
		    "org.quickshell.test",
		    "Authentication is required to run the test.",
		    "",
		    details,
		    "test-cookie",
		    identities,
		    this->cancellable,
		    &FakePolkit::onAnswer,
		    this
		);

		g_object_unref(details);
		g_list_free_full(identities, g_object_unref);
	}

	// polkitd sends CancelAuthentication when, for example, the requesting
	// process leaves the bus.
	void cancel() const {
		if (!this->answered) g_cancellable_cancel(this->cancellable);
	}

	GCancellable* cancellable = g_cancellable_new();
	bool answered = false;
	bool granted = false;
	bool cancelled = false;

private:
	static void onAnswer(GObject* source, GAsyncResult* result, gpointer userData) {
		auto* self = static_cast<FakePolkit*>(userData);
		GError* error = nullptr;

		self->granted = polkit_agent_listener_initiate_authentication_finish(
		    POLKIT_AGENT_LISTENER(source),
		    result,
		    &error
		);

		self->cancelled = g_error_matches(error, POLKIT_ERROR, POLKIT_ERROR_CANCELLED);
		g_clear_error(&error);
		self->answered = true;
		g_clear_object(&self->cancellable);
	}

	QsPolkitAgent* listener;
};

void iterate() {
	while (g_main_context_iteration(nullptr, false)) {}
}

} // namespace

// Two overlapping requests: the first one's completion retires the second
// flow, which deletes its request without answering it. When the second
// requester is killed, polkitd cancels its still pending request, and the
// cancel handler must not run on the deleted request.
void TestPolkitListener::cancelAfterUnansweredDelete() {
	auto agent = FakeAgent();
	auto* listener = qs_polkit_agent_new(&agent);
	auto polkit = FakePolkit(listener);

	polkit.begin();
	QCOMPARE(agent.started.size(), 1);

	// Keep the task so the test can release the requester at the end;
	// nothing in the agent answers a request deleted this way.
	auto* task = G_TASK(g_object_ref(agent.started.first()->task.get()));
	delete agent.started.first();

	polkit.cancel();
	iterate();
	QVERIFY(agent.cancelled.isEmpty());

	g_task_return_boolean(task, false);
	g_object_unref(task);
	iterate();
	QVERIFY(polkit.answered);

	g_object_unref(listener);
}

// The ordinary path: the request is answered, then deleted later with its
// flow. libpolkit-agent drops its reference on the cancellable with the
// answer, and the request must release its task so the listener, which
// every task holds, can be finalized.
void TestPolkitListener::cancelAfterCompletion() {
	auto agent = FakeAgent();
	auto* listener = qs_polkit_agent_new(&agent);
	auto* finalized = listener;
	g_object_add_weak_pointer(G_OBJECT(listener), reinterpret_cast<gpointer*>(&finalized));

	{
		auto polkit = FakePolkit(listener);
		polkit.begin();
		QCOMPARE(agent.started.size(), 1);

		auto* request = agent.started.first();
		request->complete();
		iterate();
		QVERIFY(polkit.answered);
		QVERIFY(polkit.granted);

		delete request;
		iterate();
		QVERIFY(agent.cancelled.isEmpty());
	}

	g_object_unref(listener);
	QVERIFY(finalized == nullptr);
}

// PolkitAgentImpl deletes a queued request from inside its own cancel
// handler. Disconnecting from there must not block, and the requester must
// still get its answer.
void TestPolkitListener::deleteFromCancelHandler() {
	auto agent = FakeAgent();
	agent.deleteOnCancel = true;
	auto* listener = qs_polkit_agent_new(&agent);
	auto polkit = FakePolkit(listener);

	polkit.begin();
	QCOMPARE(agent.started.size(), 1);

	polkit.cancel();
	iterate();
	QCOMPARE(agent.cancelled.size(), 1);
	QVERIFY(polkit.answered);
	QVERIFY(polkit.cancelled);

	g_object_unref(listener);
}

// A cancellable that is already cancelled runs the handler inside
// g_cancellable_connect, before the agent has seen the request. The request
// must be refused rather than started. libpolkit-agent does not pass a
// cancelled cancellable today; this follows GCancellable's contract.
void TestPolkitListener::alreadyCancelled() {
	auto agent = FakeAgent();
	auto* listener = qs_polkit_agent_new(&agent);
	auto polkit = FakePolkit(listener);

	g_cancellable_cancel(polkit.cancellable);
	polkit.begin();
	iterate();

	QVERIFY(agent.started.isEmpty());
	QVERIFY(polkit.answered);
	QVERIFY(polkit.cancelled);

	g_object_unref(listener);
}

// The listener can outlive its agent (a registration in flight holds it).
// A request arriving after the agent is gone must be refused with a
// cancelled task rather than handed to it.
void TestPolkitListener::requestAfterDetach() {
	auto agent = FakeAgent();
	auto* listener = qs_polkit_agent_new(&agent);
	auto polkit = FakePolkit(listener);

	qs_polkit_agent_detach(listener);
	polkit.begin();
	iterate();

	QVERIFY(agent.started.isEmpty());
	QVERIFY(polkit.answered);
	QVERIFY(polkit.cancelled);

	g_object_unref(listener);
}

QTEST_GUILESS_MAIN(TestPolkitListener);
