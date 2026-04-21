#pragma once
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <queue>

//Acts a light weight pub-sub mechanism
//Making it templatized make is to be able to be used as a type safe mechanism
template<class Key, class SubscriberId, class... Data>
struct PubSubDataRouter final
{

	PubSubDataRouter() = default;
	PubSubDataRouter(const PubSubDataRouter&) = delete;
	PubSubDataRouter& operator=(const PubSubDataRouter&) = delete;
	PubSubDataRouter(PubSubDataRouter&&) = delete;
	PubSubDataRouter& operator=(PubSubDataRouter&&) = delete;

	typedef std::function<void(const Data&...)> DataCallback;
	typedef std::function<void(const Key&, const Data&...)> KeyedDataCallback;
	//Produce Data with the specified key
	bool produce(const Key& key, const Data&... data)
	{
		bool retVal = false;
		if (auto it = m_routingTable.find(key); it != m_routingTable.end()) {
			retVal = true;
			for (auto const& [registrationId, callback] : it->second) {
				callback(data...);
			}
		}
		for (auto const& [subscriberId, callback] : m_everythingSubscribers) {
			callback(key, data...);
			retVal = true;
		}
		return retVal;
	}


	bool consume(const Key& key, const SubscriberId& subscriberId, const DataCallback& callback)
	{
		bool retVal = true;
		if (auto it = m_routingTable.find(key); it != m_routingTable.end())
			if (auto itCallback = it->second.find(subscriberId); itCallback == it->second.end()) {
				it->second[subscriberId] = callback;
				m_regIdToKeys[subscriberId].insert(key);
			}
			else
				retVal = false;
		else {
			m_routingTable[key] = { {subscriberId, callback} };
			m_regIdToKeys[subscriberId].insert(key);
		}

		return retVal;
	}

	bool consumeEveryThing(const SubscriberId& subscriberId, const KeyedDataCallback& callback)
	{
		if (m_everythingSubscribers.find(subscriberId) != m_everythingSubscribers.end())
			return false;
		m_everythingSubscribers[subscriberId] = callback;
		return true;
	}

	bool unregisterFromEverything(const SubscriberId& subscriberId)
	{
		return m_everythingSubscribers.erase(subscriberId) > 0;
	}

	bool unregister(const Key& key, const SubscriberId& subscriberId)
	{
		return removeFromRoutingTable(key, subscriberId) &&
			   removeFromRegIdStore(key, subscriberId);
	}

	bool unregisterAll(const SubscriberId& subscriberId)
	{
		bool retVal = false;
		if (auto it = m_regIdToKeys.find(subscriberId); it != m_regIdToKeys.end()) {
			auto const& [regId, keys] = *it;
			for (auto const& key : keys) {
				removeFromRoutingTable(key, subscriberId);
			}
			m_regIdToKeys.erase(it);
			retVal = true;
		}
		if (m_everythingSubscribers.erase(subscriberId) > 0)
			retVal = true;
		return retVal;
	}

private:
	bool removeFromRoutingTable(const Key& key, const SubscriberId& subscriberId) {
		bool retVal = false;
		if (auto it = m_routingTable.find(key); it != m_routingTable.end()) {
			if (auto itCallback = it->second.find(subscriberId); itCallback != it->second.end()) {
				it->second.erase(itCallback);
				if (it->second.empty()) {
					m_routingTable.erase(it);
				}

				retVal = true;
			}
		}

		return retVal;
	}

	bool removeFromRegIdStore(const Key& key, const SubscriberId& subscriberId) {
		auto it = m_regIdToKeys.find(subscriberId);
		if (it == m_regIdToKeys.end()) {
			return false;
		}

		auto& [regId, keys] = *it;
		if(0 == keys.erase(key)) {
			return false;
		}

		if (keys.empty()) {
			m_regIdToKeys.erase(it);
		}

		return true;
	}

	std::unordered_map<Key, std::unordered_map<SubscriberId, DataCallback>> m_routingTable;
	std::unordered_map<SubscriberId, std::unordered_set<Key>> m_regIdToKeys;
	std::unordered_map<SubscriberId, KeyedDataCallback> m_everythingSubscribers;
};

//Acts a light weight request-response mechanism
//Making it templatized make is to be able to be used as a type safe mechanism'
template<class ResponderId, class ReqId, class ReqData, class... Response>
struct EndToEndReqRespRouter {

	EndToEndReqRespRouter(const EndToEndReqRespRouter&) = delete;
	EndToEndReqRespRouter& operator=(const EndToEndReqRespRouter&) = delete;
	EndToEndReqRespRouter(EndToEndReqRespRouter&&) = delete;
	EndToEndReqRespRouter& operator=(EndToEndReqRespRouter&&) = delete;

	typedef std::function<void(bool, const Response&...)> ResponseHandler;
	typedef std::function<void(const ReqData&, const ResponseHandler&)> ReqListener;
	typedef PubSubDataRouter<ResponderId, size_t, ReqData, ResponseHandler> ReqRouter;
	typedef PubSubDataRouter<ReqId, size_t, Response...> RespRouter;

	EndToEndReqRespRouter() : m_reqPending(false) {}

	bool registerAsResponder(const ResponderId& responderId, const ReqListener& reqListener) {
		return m_reqRouter.consume(responderId,
									(size_t)this,
				[this, reqListener](const ReqData& reqData,
									const ResponseHandler& responseHandler) {
					reqListener(reqData, responseHandler);
				}
		);
	}

	bool unregisterAsResponder(const ResponderId& responderId) {
		return m_reqRouter.unregister(responderId, (size_t)this);
	}

	bool request(const ResponderId& responderId,
				 const ReqId& reqId,
				 const ReqData& reqData,
				 const ResponseHandler& responseHandler) {
		if (m_pendingreqIds.find(reqId) != m_pendingreqIds.end()) {
			return false;
		}

		m_pendingreqIds.insert(reqId);
		auto reqForwarder =
		[this, responderId, reqId, reqData, responseHandler](){
			m_reqPending = true;
			m_reqRouter.produce(responderId, reqData, [this, reqId, responseHandler](bool isLast, const Response&... response) {
				if (auto it = m_pendingreqIds.find(reqId); it != m_pendingreqIds.end()) {
					responseHandler(isLast, response...);
					if (isLast) m_pendingreqIds.erase(it);
				}
			});
			m_reqPending = false;
		};

		if (m_reqPending) {
			m_pendingReqProcessors.push(reqForwarder);
		} else {
			reqForwarder();
			while (!m_pendingReqProcessors.empty()) {
				m_pendingReqProcessors.front()();
				m_pendingReqProcessors.pop();
			}
		}

		return true;
	}

	bool cancelRequest(const ReqId& reqId) {
		bool retVal = false;
		if (auto it = m_pendingreqIds.find(reqId); it != m_pendingreqIds.end()) {
			m_pendingreqIds.erase(it);
			retVal = true;
		}

		return retVal;
	}

	private:
	bool m_reqPending;
	std::queue<std::function<void()>> m_pendingReqProcessors;
	ReqRouter m_reqRouter;
	std::unordered_set<ReqId> m_pendingreqIds;
};

template<class ResponderId, class ReqType, class ReqId, class ReqData, class... Response>
struct GenericReqRespRouter final{

	GenericReqRespRouter(const GenericReqRespRouter&) = delete;
	GenericReqRespRouter& operator=(const GenericReqRespRouter&) = delete;
	GenericReqRespRouter(GenericReqRespRouter&&) = delete;
	GenericReqRespRouter& operator=(GenericReqRespRouter&&) = delete;

	typedef std::function<void(bool, const Response&...)> ResponseHandler;
	typedef std::function<void(const ReqData&, const ResponseHandler&)> ReqListener;
	typedef PubSubDataRouter<ReqType, size_t, ReqData, ResponseHandler> ReqRouter;
	typedef PubSubDataRouter<ReqId, size_t, Response...> RespRouter;
	GenericReqRespRouter() : m_reqPending(false) {}

	bool registerAsResponder(const ResponderId& responderId, const ReqType& reqType, const ReqListener& reqListener) {
		if(auto it = m_responderBook.find(reqType); it == m_responderBook.end()) {
			m_responderBook[reqType] = responderId;
			m_reqRouter.consume(reqType, (size_t)this, reqListener);
			return true;
		} else {
			return false;
		}
	}

	bool unregisterAsResponder(const ResponderId& responderId, const ReqType& reqType) {
		if( auto it = m_responderBook.find(reqType); it != m_responderBook.end()) {
			m_responderBook.erase(it);
			m_reqRouter.unregister(reqType, (size_t)this);
			return true;
		} else {
			return false;
		}
	}

	bool request(const ReqId& reqId,
				 const ReqType& reqType,
				 const ReqData& reqData,	
				 const ResponseHandler& responseHandler) {
		if (m_pendingreqIds.find(reqId) != m_pendingreqIds.end()) {
			return false;
		}

		m_pendingreqIds.insert(reqId);
		auto reqForwarder =
		[this, reqId, reqType, reqData, responseHandler](){
			m_reqPending = true;
			m_reqRouter.produce(reqType, reqData, [this, reqId, responseHandler](bool isLast, const Response&... response) {
				if (auto it = m_pendingreqIds.find(reqId); it != m_pendingreqIds.end()) {
					responseHandler(isLast, response...);
					if (isLast) m_pendingreqIds.erase(it);
				}
			});
			m_reqPending = false;
		};

		if (m_reqPending) {
			m_pendingReqProcessors.push(reqForwarder);
		} else {
			reqForwarder();
			while (!m_pendingReqProcessors.empty()) {
				m_pendingReqProcessors.front()();
				m_pendingReqProcessors.pop();
			}
		}

		return true;
	}

	bool cancelRequest(const ReqId& reqId) {
		bool retVal = false;
		if (auto it = m_pendingreqIds.find(reqId); it != m_pendingreqIds.end()) {
			m_pendingreqIds.erase(it);
			retVal = true;
		}

		return retVal;
	}

	private:
	bool m_reqPending;
	ReqRouter m_reqRouter;
	std::queue<std::function<void()>> m_pendingReqProcessors;
	std::unordered_set<ReqId> m_pendingreqIds;
	std::unordered_map<ReqType, ResponderId> m_responderBook;
};

