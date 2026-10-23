#include "agent.hpp"

#include <glib-object.h>
#include <glib.h>
#include <qcoreapplication.h>
#include <qcoreevent.h>
#include <qdeadlinetimer.h>
#include <qlist.h>
#include <qlogging.h>
#include <qobject.h>
#include <qstring.h>
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

#include "../agentimpl.hpp"
#include "../flow.hpp"
#include "../listener.hpp"
#include "../qml.hpp"

using qs::service::polkit::AuthFlow;
using qs::service::polkit::PolkitAgent;
using qs::service::polkit::PolkitAgentImpl;

namespace {

// Everything these tests leave on the main loop: Qt's posted events and
// deferred deletes, and the GLib sources polkit and the listener use.
void iterate() {
	QCoreApplication::processEvents();
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	while (g_main_context_iteration(nullptr, false)) {}
}

template <typename Pred>
bool waitFor(Pred pred, int timeoutMs = 5000) {
	auto deadline = QDeadlineTimer(timeoutMs);
	while (!pred()) {
		if (deadline.hasExpired()) return false;
		iterate();
		QTest::qSleep(1);
	}
	return true;
}

// The agent registers with polkitd as soon as it exists. There is no polkitd
// here, so the registration fails, from a worker thread a moment later. The
// harness waits for that answer before a test starts, so it cannot arrive
// after the test has torn the agent down.
bool registrationAnswered = false;
QtMessageHandler previousHandler = nullptr;

void messageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
	if (message.contains("failed to create subject for listener")
	    || message.contains("failed to register listener"))
	{
		registrationAnswered = true;
	}

	if (previousHandler) previousHandler(type, context, message);
}

// What the QML side sees in one authenticationRequestStarted: the flow the
// agent exposes, the flow the implementation holds, and their cookies.
struct Started {
	AuthFlow* qmlFlow;
	AuthFlow* implFlow;
	QString qmlCookie;
	QString cookie;
};

// A PolkitAgent as QML would create it, with its implementation. Requests
// reach the implementation through a second listener with the same
// callbacks as its own, which is private; the requests are built by the
// same code either way.
class Agent {
public:
	Agent() {
		registrationAnswered = false;
		this->qml = new PolkitAgent();
		this->qml->componentComplete();
		this->impl = PolkitAgentImpl::tryGet(this->qml);
		this->listener = qs_polkit_agent_new(this->impl);

		QObject::connect(this->qml, &PolkitAgent::authenticationRequestStarted, this->qml, [this]() {
			auto* qmlFlow = this->qml->flow().value();
			auto* implFlow = this->impl->activeFlow().value();
			this->started.append({
			    .qmlFlow = qmlFlow,
			    .implFlow = implFlow,
			    .qmlCookie = qmlFlow ? qmlFlow->cookie() : QString(),
			    .cookie = implFlow ? implFlow->cookie() : QString(),
			});
		});

		this->ready =
		    waitFor([this]() { return registrationAnswered || this->qml->isRegistered().value(); });
	}

	~Agent() { this->destroy(); }
	Q_DISABLE_COPY_MOVE(Agent);

	// Ends the agent the way a QML reload does, and lets the answers to its
	// outstanding requests through.
	void destroy() {
		if (!this->qml) return;
		delete this->qml;
		this->qml = nullptr;
		this->impl = nullptr;
		iterate();
		g_object_unref(this->listener);
	}

	PolkitAgent* qml = nullptr;
	PolkitAgentImpl* impl = nullptr;
	QsPolkitAgent* listener = nullptr;
	QList<Started> started;
	bool ready = false;
};

// Stands in for polkitd's side of one request (polkitagentlistener.c): a
// fresh cancellable per request, dropped when the agent answers, and a
// CancelAuthentication that only reaches a request still waiting.
class FakePolkit {
public:
	FakePolkit(QsPolkitAgent* listener, const char* cookie, const char* message)
	    : listener(listener)
	    , cookie(cookie)
	    , message(message) {}

	~FakePolkit() { g_clear_object(&this->cancellable); }
	Q_DISABLE_COPY_MOVE(FakePolkit);

	void begin() {
		auto* identities = g_list_append(nullptr, polkit_unix_user_new(0));
		auto* details = polkit_details_new();

		polkit_agent_listener_initiate_authentication(
		    POLKIT_AGENT_LISTENER(this->listener),
		    "org.quickshell.test",
		    this->message,
		    "",
		    details,
		    this->cookie,
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
	bool cancelled = false;

private:
	static void onAnswer(GObject* source, GAsyncResult* result, gpointer userData) {
		auto* self = static_cast<FakePolkit*>(userData);
		GError* error = nullptr;

		polkit_agent_listener_initiate_authentication_finish(
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
	const char* cookie;
	const char* message;
};

} // namespace

void TestPolkitAgent::initTestCase() { previousHandler = qInstallMessageHandler(&messageHandler); }

// A flow completes inside a property update group (AuthFlow::completed and
// the cancel paths all open one), and PolkitAgent.flow is a binding on the
// implementation's active flow, which the group holds back until it ends.
// If the next queued request is activated from the completion, the
// authenticationRequestStarted it emits is seen with PolkitAgent.flow still
// reading the finished flow: the wrong message, action and cookie for the
// prompt. The agent must start the next request with its own flow visible.
void TestPolkitAgent::chainedRequestStartsWithItsOwnFlow() {
	auto agent = Agent();
	QVERIFY(agent.ready);
	auto first = FakePolkit(agent.listener, "cookie-1", "First request");
	auto second = FakePolkit(agent.listener, "cookie-2", "Second request");

	first.begin();
	QCOMPARE(agent.started.size(), 1);
	auto* firstFlow = agent.impl->activeFlow().value();
	QVERIFY(firstFlow);
	QCOMPARE(agent.qml->flow().value(), firstFlow);

	second.begin();
	QCOMPARE(agent.started.size(), 1);
	QCOMPARE(agent.impl->activeFlow().value(), firstFlow);

	// polkitd withdraws the first request; its flow completes inside the
	// cancel's property update group.
	first.cancel();
	iterate();
	QCOMPARE(agent.started.size(), 2);

	auto* secondFlow = agent.impl->activeFlow().value();
	QVERIFY(secondFlow);
	QVERIFY(secondFlow != firstFlow);
	QCOMPARE(secondFlow->cookie(), QString("cookie-2"));

	const auto& start = agent.started[1];
	QCOMPARE(start.implFlow, secondFlow);
	QCOMPARE(start.qmlCookie, QString("cookie-2"));
	QCOMPARE(start.qmlFlow, secondFlow);
	QCOMPARE(agent.qml->flow().value(), secondFlow);

	QVERIFY(first.answered);
	QVERIFY(first.cancelled);
	QVERIFY(!second.answered);

	agent.destroy();
	QVERIFY(second.answered);
	QVERIFY(second.cancelled);
}

// Between a completion and the activation it hands to the event loop there
// is no active flow. A request arriving in that window activates the head
// of the queue itself, which is the request queued earlier, not its own,
// and the activation that then runs from the event loop finds a flow active
// and leaves it alone.
void TestPolkitAgent::requestArrivingBeforeChainedStartKeepsOrder() {
	auto agent = Agent();
	QVERIFY(agent.ready);
	auto first = FakePolkit(agent.listener, "cookie-1", "First request");
	auto second = FakePolkit(agent.listener, "cookie-2", "Second request");
	auto third = FakePolkit(agent.listener, "cookie-3", "Third request");

	first.begin();
	second.begin();
	auto* firstFlow = agent.impl->activeFlow().value();
	QVERIFY(firstFlow);

	first.cancel();
	QVERIFY(!agent.impl->activeFlow().value());
	QVERIFY(!agent.qml->flow().value());
	QVERIFY(!agent.qml->isActive().value());

	third.begin();
	QCOMPARE(agent.started.size(), 2);
	QCOMPARE(agent.started[1].cookie, QString("cookie-2"));
	QCOMPARE(agent.started[1].qmlFlow, agent.started[1].implFlow);

	iterate();
	QCOMPARE(agent.started.size(), 2);
	QVERIFY(agent.impl->activeFlow().value());
	QCOMPARE(agent.impl->activeFlow().value()->cookie(), QString("cookie-2"));

	second.cancel();
	iterate();
	QCOMPARE(agent.started.size(), 3);
	QCOMPARE(agent.started[2].cookie, QString("cookie-3"));
	QCOMPARE(agent.started[2].qmlCookie, QString("cookie-3"));
	QCOMPARE(agent.started[2].qmlFlow, agent.started[2].implFlow);

	agent.destroy();
	QVERIFY(first.cancelled);
	QVERIFY(second.cancelled);
	QVERIFY(third.cancelled);
}

// polkitd withdraws a queued request before the activation that would have
// started it runs. The activation then finds nothing to start, and the
// request is answered as cancelled.
void TestPolkitAgent::queuedRequestCancelledBeforeChainedStart() {
	auto agent = Agent();
	QVERIFY(agent.ready);
	auto first = FakePolkit(agent.listener, "cookie-1", "First request");
	auto second = FakePolkit(agent.listener, "cookie-2", "Second request");
	auto third = FakePolkit(agent.listener, "cookie-3", "Third request");

	first.begin();
	second.begin();
	first.cancel();
	second.cancel();
	iterate();

	QVERIFY(!agent.impl->activeFlow().value());
	QVERIFY(!agent.qml->flow().value());
	QVERIFY(!agent.qml->isActive().value());
	QVERIFY(second.answered);
	QVERIFY(second.cancelled);

	third.begin();
	QVERIFY(agent.impl->activeFlow().value());
	QCOMPARE(agent.started.last().cookie, QString("cookie-3"));
	QCOMPARE(agent.started.last().qmlFlow, agent.started.last().implFlow);

	agent.destroy();
	QVERIFY(third.cancelled);
}

// The agent goes away, as on a QML reload, while an activation it handed to
// the event loop is still pending. The pending call must not reach the
// destroyed object, and the queued request is answered as cancelled.
void TestPolkitAgent::agentDestroyedBeforeChainedStart() {
	auto agent = Agent();
	QVERIFY(agent.ready);
	auto first = FakePolkit(agent.listener, "cookie-1", "First request");
	auto second = FakePolkit(agent.listener, "cookie-2", "Second request");

	first.begin();
	second.begin();
	first.cancel();
	agent.destroy();

	QVERIFY(first.answered);
	QVERIFY(first.cancelled);
	QVERIFY(second.answered);
	QVERIFY(second.cancelled);

	// The agent that replaces it is unaffected by the dropped call.
	auto next = Agent();
	QVERIFY(next.ready);
	auto third = FakePolkit(next.listener, "cookie-3", "Third request");

	third.begin();
	QCOMPARE(next.started.size(), 1);
	QCOMPARE(next.started.first().cookie, QString("cookie-3"));

	next.destroy();
	QVERIFY(third.answered);
	QVERIFY(third.cancelled);
}

// A completion with nothing queued leaves no flow and starts nothing.
void TestPolkitAgent::completionWithEmptyQueue() {
	auto agent = Agent();
	QVERIFY(agent.ready);
	auto first = FakePolkit(agent.listener, "cookie-1", "First request");

	first.begin();
	first.cancel();
	iterate();

	QVERIFY(!agent.impl->activeFlow().value());
	QVERIFY(!agent.qml->flow().value());
	QCOMPARE(agent.started.size(), 1);
	QVERIFY(first.answered);
	QVERIFY(first.cancelled);
}

QTEST_GUILESS_MAIN(TestPolkitAgent);
