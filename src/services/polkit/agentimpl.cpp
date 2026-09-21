#include "agentimpl.hpp"
#include <algorithm>
#include <utility>

#include <qlist.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qproperty.h>
#include <qtmetamacros.h>

#include "../../core/generation.hpp"
#include "../../core/logcat.hpp"
#include "gobjectref.hpp"
#include "listener.hpp"
#include "qml.hpp"

namespace {
QS_LOGGING_CATEGORY(logPolkit, "quickshell.service.polkit", QtWarningMsg);
}

namespace qs::service::polkit {
PolkitAgentImpl* PolkitAgentImpl::instance = nullptr;

PolkitAgentImpl::PolkitAgentImpl(PolkitAgent* agent)
    : QObject(nullptr)
    , listener(qs_polkit_agent_new(this), G_OBJECT_NO_REF)
    , qmlAgent(agent)
    , path(this->qmlAgent->path()) {
	auto utf8Path = this->path.toUtf8();
	qs_polkit_agent_register(this->listener.get(), utf8Path.constData());
}

PolkitAgentImpl::~PolkitAgentImpl() {
	this->cancelAllRequests("PolkitAgent is being destroyed");
	// The listener may outlive this object (a registration in flight holds
	// a reference to it): forget the callback so nothing dispatches here.
	qs_polkit_agent_detach(this->listener.get());
}

void PolkitAgentImpl::cancelAllRequests(const QString& reason) {
	for (; !this->queuedRequests.empty(); this->queuedRequests.pop_back()) {
		AuthRequest* req = this->queuedRequests.back();
		qCDebug(logPolkit) << "destroying queued authentication request for action" << req->actionId;
		req->cancel(reason);
		delete req;
	}

	auto* flow = this->bActiveFlow.value();
	if (flow) {
		QObject::disconnect(flow, nullptr, this, nullptr);
		this->bActiveFlow = nullptr;
		flow->cancelAuthenticationRequest();
		// The flow and its request live until the deferred delete runs, with
		// the cancel handler still connected; a cancel in that window must
		// not reach this object, which may be gone by then.
		if (auto* req = flow->authRequest()) req->cb = nullptr;
		flow->deleteLater();
	}

	// A no-op without a registration handle, so the bIsRegistered gate
	// bought nothing; a registration still in flight is stopped by the
	// detach check in the registration callback.
	qs_polkit_agent_unregister(this->listener.get());
}

PolkitAgentImpl* PolkitAgentImpl::tryGetOrCreate(PolkitAgent* agent) {
	if (instance == nullptr) instance = new PolkitAgentImpl(agent);
	if (instance->qmlAgent == agent) return instance;
	return nullptr;
}

PolkitAgentImpl* PolkitAgentImpl::tryGet(const PolkitAgent* agent) {
	if (instance == nullptr) return nullptr;
	if (instance->qmlAgent == agent) return instance;
	return nullptr;
}

PolkitAgentImpl* PolkitAgentImpl::tryTakeoverOrCreate(PolkitAgent* agent) {
	if (auto* impl = tryGetOrCreate(agent); impl != nullptr) return impl;

	auto* prevGen = EngineGeneration::findObjectGeneration(instance->qmlAgent);
	auto* myGen = EngineGeneration::findObjectGeneration(agent);
	if (prevGen == myGen) return nullptr;

	qCDebug(logPolkit) << "taking over listener from previous generation";
	instance->qmlAgent = agent;
	instance->setPath(agent->path());

	return instance;
}

void PolkitAgentImpl::onEndOfQmlAgent(PolkitAgent* agent) {
	if (instance != nullptr && instance->qmlAgent == agent) {
		delete instance;
		instance = nullptr;
	}
}

void PolkitAgentImpl::setPath(const QString& path) {
	if (this->path == path) return;

	this->path = path;
	auto utf8Path = path.toUtf8();

	this->cancelAllRequests("PolkitAgent path changed");
	qs_polkit_agent_unregister(this->listener.get());
	this->bIsRegistered = false;

	qs_polkit_agent_register(this->listener.get(), utf8Path.constData());
}

void PolkitAgentImpl::registerComplete(bool success) {
	if (success) this->bIsRegistered = true;
	else qCWarning(logPolkit) << "failed to register listener on path" << this->qmlAgent->path();
}

void PolkitAgentImpl::initiateAuthentication(AuthRequest* request) {
	qCDebug(logPolkit) << "incoming authentication request for action" << request->actionId;

	this->queuedRequests.emplace_back(request);

	// Activation pops the request it activates, so the queue is empty again
	// after every activation and its size says nothing about whether a flow
	// is in progress. Gate on the flow itself.
	if (this->bActiveFlow.value() == nullptr) {
		this->activateAuthenticationRequest();
	}
}

void PolkitAgentImpl::cancelAuthentication(AuthRequest* request) {
	qCDebug(logPolkit) << "cancelling authentication request from agent";

	auto* flow = this->bActiveFlow.value();
	if (flow && flow->authRequest() == request) {
		flow->cancelFromAgent();
	} else if (
	    auto it = std::ranges::find(this->queuedRequests, request); it != this->queuedRequests.end()
	)
	{
		qCDebug(logPolkit) << "removing queued authentication request for action" << (*it)->actionId;
		(*it)->cancel("Authentication request was cancelled");
		delete (*it);
		this->queuedRequests.erase(it);
	} else {
		qCWarning(logPolkit) << "the cancelled request was not found in the queue.";
	}
}

void PolkitAgentImpl::activateAuthenticationRequest() {
	// Walk the queue until a request can be activated; on every exit
	// bActiveFlow is accurate, so the gate above never sees a stale flow.
	while (!this->queuedRequests.empty()) {
		AuthRequest* req = this->queuedRequests.front();
		this->queuedRequests.pop_front();
		qCDebug(logPolkit) << "activating authentication request for action" << req->actionId
		                   << ", cookie: " << req->cookie;

		QList<Identity*> identities;
		for (auto& identity: req->identities) {
			auto* obj = Identity::fromPolkitIdentity(identity);
			if (obj) identities.append(obj);
		}
		if (identities.isEmpty()) {
			qCWarning(
			    logPolkit
			) << "no supported identities available for authentication request, cancelling.";
			req->cancel("Error requesting authentication: no supported identities available.");
			delete req;
			continue;
		}

		this->bActiveFlow = new AuthFlow(req, std::move(identities));

		QObject::connect(
		    this->bActiveFlow.value(),
		    &AuthFlow::isCompletedChanged,
		    this,
		    &PolkitAgentImpl::finishAuthenticationRequest
		);

		emit this->qmlAgent->authenticationRequestStarted();
		return;
	}

	this->bActiveFlow = nullptr;
}

void PolkitAgentImpl::finishAuthenticationRequest() {
	if (!this->bActiveFlow.value()) return;

	qCDebug(logPolkit) << "finishing authentication request for action"
	                   << this->bActiveFlow.value()->actionId();

	QObject::disconnect(this->bActiveFlow.value(), nullptr, this, nullptr);
	// The finished flow's request outlives it until the deferred delete,
	// with its cancel handler still connected; a cancel arriving in that
	// window has nothing to act on and must not reach an object that may
	// be gone by then.
	if (auto* req = this->bActiveFlow.value()->authRequest()) req->cb = nullptr;
	this->bActiveFlow.value()->deleteLater();
	this->bActiveFlow = nullptr;

	// activateAuthenticationRequest leaves bActiveFlow null when nothing
	// could be activated, so this is exact whether the queue is empty or not.
	this->activateAuthenticationRequest();
}
} // namespace qs::service::polkit
