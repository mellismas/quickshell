#pragma once

#include <qobject.h>
#include <qtmetamacros.h>

class TestPolkitAgent: public QObject {
	Q_OBJECT;

private slots:
	static void initTestCase();
	static void chainedRequestStartsWithItsOwnFlow();
	static void requestArrivingBeforeChainedStartKeepsOrder();
	static void queuedRequestCancelledBeforeChainedStart();
	static void agentDestroyedBeforeChainedStart();
	static void completionWithEmptyQueue();
};
