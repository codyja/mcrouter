/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <memory>

#include <folly/Conv.h>
#include <folly/Range.h>

#include "mcrouter/CarbonRouterInstanceBase.h"
#include "mcrouter/McrouterLogFailure.h"
#include "mcrouter/PoolFactory.h"
#include "mcrouter/ProxyBase.h"
#include "mcrouter/ProxyDestination.h"
#include "mcrouter/ProxyDestinationMap.h"
#include "mcrouter/config.h"
#include "mcrouter/lib/WeightedCh3HashFunc.h"
#include "mcrouter/lib/fbi/cpp/ParsingUtil.h"
#include "mcrouter/lib/fbi/cpp/util.h"
#include "mcrouter/lib/network/AccessPoint.h"
#include "mcrouter/lib/network/AsyncMcClient.h"
#include "mcrouter/lib/network/SecurityOptions.h"
#include "mcrouter/lib/network/ThriftTransport.h"
#include "mcrouter/lib/network/gen/MemcacheRouterInfo.h"
#include "mcrouter/routes/AsynclogRoute.h"
#include "mcrouter/routes/DestinationRoute.h"
#include "mcrouter/routes/ExtraRouteHandleProviderIf.h"
#include "mcrouter/routes/FailoverRoute.h"
#include "mcrouter/routes/HashRouteFactory.h"
#include "mcrouter/routes/PoolRouteUtils.h"
#include "mcrouter/routes/RateLimitRoute.h"
#include "mcrouter/routes/RateLimiter.h"
#include "mcrouter/routes/ShadowRoute.h"
#include "mcrouter/routes/ShardHashFunc.h"
#include "mcrouter/routes/ShardSplitRoute.h"
#include "mcrouter/routes/ShardSplitter.h"

namespace facebook {
namespace memcache {
namespace mcrouter {

template <class RouterInfo>
std::shared_ptr<typename RouterInfo::RouteHandleIf> makeLoggingRoute(
    RouteHandleFactory<typename RouterInfo::RouteHandleIf>& factory,
    const folly::dynamic& json);

template <class RouteHandleIf>
std::shared_ptr<RouteHandleIf> makeNullRoute(
    RouteHandleFactory<RouteHandleIf>& factory,
    const folly::dynamic& json);

template <class RouterInfo>
McRouteHandleProvider<RouterInfo>::McRouteHandleProvider(
    ProxyBase& proxy,
    PoolFactory& poolFactory)
    : proxy_(proxy),
      poolFactory_(poolFactory),
      extraProvider_(buildExtraProvider()),
      routeMap_(buildCheckedRouteMap()) {}

template <class RouterInfo>
McRouteHandleProvider<RouterInfo>::~McRouteHandleProvider() {
  /* Needed for forward declaration of ExtraRouteHandleProviderIf in .h */
}

template <class RouterInfo>
std::unique_ptr<ExtraRouteHandleProviderIf<RouterInfo>>
McRouteHandleProvider<RouterInfo>::buildExtraProvider() {
  return RouterInfo::buildExtraProvider();
}

template <>
std::unique_ptr<ExtraRouteHandleProviderIf<MemcacheRouterInfo>>
McRouteHandleProvider<MemcacheRouterInfo>::buildExtraProvider();

template <class RouterInfo>
std::shared_ptr<typename RouterInfo::RouteHandleIf>
McRouteHandleProvider<RouterInfo>::createAsynclogRoute(
    RouteHandlePtr target,
    std::string asynclogName) {
  if (!proxy_.router().opts().asynclog_disable) {
    target = makeAsynclogRoute<RouterInfo>(std::move(target), asynclogName);
  }
  asyncLogRoutes_.emplace(std::move(asynclogName), target);
  return target;
}

template <class RouterInfo>
const std::vector<std::shared_ptr<typename RouterInfo::RouteHandleIf>>&
McRouteHandleProvider<RouterInfo>::makePool(
    RouteHandleFactory<RouteHandleIf>& factory,
    const PoolFactory::PoolJson& jpool) {
  auto existingIt = pools_.find(jpool.name);
  if (existingIt != pools_.end()) {
    return existingIt->second;
  }

  auto name = jpool.name.str();
  const auto& json = jpool.json;
  auto& opts = proxy_.router().opts();
  // region & cluster
  folly::StringPiece region, cluster;
  if (auto jregion = json.get_ptr("region")) {
    if (!jregion->isString()) {
      MC_LOG_FAILURE(
          opts,
          memcache::failure::Category::kInvalidConfig,
          "Pool {}: pool_region is not a string",
          name);
    } else {
      region = jregion->stringPiece();
    }
  }
  if (auto jcluster = json.get_ptr("cluster")) {
    if (!jcluster->isString()) {
      MC_LOG_FAILURE(
          opts,
          memcache::failure::Category::kInvalidConfig,
          "Pool {}: pool_cluster is not a string",
          name);
    } else {
      cluster = jcluster->stringPiece();
    }
  }

  try {
    std::chrono::milliseconds timeout{opts.server_timeout_ms};
    if (auto jTimeout = json.get_ptr("server_timeout")) {
      timeout = parseTimeout(*jTimeout, "server_timeout");
    }

    std::chrono::milliseconds connectTimeout = timeout;
    if (auto jConnectTimeout = json.get_ptr("connect_timeout")) {
      connectTimeout = parseTimeout(*jConnectTimeout, "connect_timeout");
    }

    if (!region.empty() && !cluster.empty()) {
      auto& route = opts.default_route;
      if (region == route.getRegion() && cluster == route.getCluster()) {
        if (opts.within_cluster_timeout_ms != 0) {
          timeout = std::chrono::milliseconds(opts.within_cluster_timeout_ms);
        }
      } else if (region == route.getRegion()) {
        if (opts.cross_cluster_timeout_ms != 0) {
          timeout = std::chrono::milliseconds(opts.cross_cluster_timeout_ms);
        }
      } else {
        if (opts.cross_region_timeout_ms != 0) {
          timeout = std::chrono::milliseconds(opts.cross_region_timeout_ms);
        }
      }
    }

    mc_protocol_t protocol = mc_ascii_protocol;
    if (auto jProtocol = json.get_ptr("protocol")) {
      auto str = parseString(*jProtocol, "protocol");
      if (equalStr("ascii", str, folly::AsciiCaseInsensitive())) {
        protocol = mc_ascii_protocol;
      } else if (equalStr("caret", str, folly::AsciiCaseInsensitive())) {
        protocol = mc_caret_protocol;
      } else if (equalStr("thrift", str, folly::AsciiCaseInsensitive())) {
        protocol = mc_thrift_protocol;
      } else {
        throwLogic("Unknown protocol '{}'", str);
      }
    }

    bool enableCompression = proxy_.router().opts().enable_compression;
    if (auto jCompression = json.get_ptr("enable_compression")) {
      enableCompression = parseBool(*jCompression, "enable_compression");
    }

    bool keepRoutingPrefix = false;
    if (auto jKeepRoutingPrefix = json.get_ptr("keep_routing_prefix")) {
      keepRoutingPrefix = parseBool(*jKeepRoutingPrefix, "keep_routing_prefix");
    }

    uint32_t qosClass = opts.default_qos_class;
    uint32_t qosPath = opts.default_qos_path;
    if (auto jQos = json.get_ptr("qos")) {
      checkLogic(jQos->isObject(), "qos must be an object.");
      if (auto jClass = jQos->get_ptr("class")) {
        qosClass = parseInt(*jClass, "qos.class", 0, 4);
      }
      if (auto jPath = jQos->get_ptr("path")) {
        qosPath = parseInt(*jPath, "qos.path", 0, 3);
      }
    }

    SecurityMech mech = SecurityMech::NONE;
    folly::Optional<SecurityMech> withinDcMech;
    folly::Optional<SecurityMech> crossDcMech;
    folly::Optional<uint16_t> crossDcPort;
    folly::Optional<uint16_t> withinDcPort;
    // default to 0, which doesn't override
    uint16_t port = 0;
    if (proxy_.router().configApi().enableSecurityConfig()) {
      if (auto jSecurityMech = json.get_ptr("security_mech")) {
        auto mechStr = parseString(*jSecurityMech, "security_mech");
        mech = parseSecurityMech(mechStr);
      } else if (auto jUseSsl = json.get_ptr("use_ssl")) {
        // deprecated - prefer security_mech
        auto useSsl = parseBool(*jUseSsl, "use_ssl");
        if (useSsl) {
          mech = SecurityMech::TLS;
        }
      }

      if (auto jSecurityMech = json.get_ptr("security_mech_within_dc")) {
        auto mechStr = parseString(*jSecurityMech, "security_mech_within_dc");
        withinDcMech = parseSecurityMech(mechStr);
      }

      if (auto jSecurityMech = json.get_ptr("security_mech_cross_dc")) {
        auto mechStr = parseString(*jSecurityMech, "security_mech_cross_dc");
        crossDcMech = parseSecurityMech(mechStr);
      }

      if (auto jPort = json.get_ptr("port_override_within_dc")) {
        withinDcPort = parseInt(*jPort, "port_override_within_dc", 1, 65535);
      }

      if (auto jPort = json.get_ptr("port_override_cross_dc")) {
        crossDcPort = parseInt(*jPort, "port_override_cross_dc", 1, 65535);
      }

      if (auto jPort = json.get_ptr("port_override")) {
        port = parseInt(*jPort, "port_override", 1, 65535);
      }
    }
    // servers
    auto jservers = json.get_ptr("servers");
    auto jhostnames = json.get_ptr("hostnames");
    checkLogic(jservers, "servers not found");
    checkLogic(jservers->isArray(), "servers is not an array");
    checkLogic(
        !jhostnames || jhostnames->isArray(), "hostnames is not an array");
    checkLogic(
        !jhostnames || jhostnames->size() == jservers->size(),
        "hostnames expected to be of the same size as servers, "
        "expected {}, got {}",
        jservers->size(),
        jhostnames ? jhostnames->size() : 0);

    int32_t poolStatIndex = proxy_.router().getStatsEnabledPoolIndex(name);

    std::vector<RouteHandlePtr> destinations;
    destinations.reserve(jservers->size());
    for (size_t i = 0; i < jservers->size(); ++i) {
      const auto& server = jservers->at(i);
      checkLogic(
          server.isString() || server.isObject(),
          "server #{} is not a string/object",
          i);
      if (server.isObject()) {
        destinations.push_back(factory.create(server));
        continue;
      }

      auto ap = AccessPoint::create(
          server.stringPiece(), protocol, mech, port, enableCompression);
      checkLogic(ap != nullptr, "invalid server {}", server.stringPiece());

      if (withinDcMech.hasValue() || crossDcMech.hasValue() ||
          withinDcPort.hasValue() || crossDcPort.hasValue()) {
        bool isInLocalDc = isInLocalDatacenter(ap->getHost());
        if (isInLocalDc) {
          if (withinDcMech.hasValue()) {
            ap->setSecurityMech(withinDcMech.value());
          }
          if (withinDcPort.hasValue()) {
            ap->setPort(withinDcPort.value());
          }
        } else {
          if (crossDcMech.hasValue()) {
            ap->setSecurityMech(crossDcMech.value());
          }
          if (crossDcPort.hasValue()) {
            ap->setPort(crossDcPort.value());
          }
        }
      }

      if (ap->compressed() && proxy_.router().getCodecManager() == nullptr) {
        if (!initCompression(proxy_.router())) {
          MC_LOG_FAILURE(
              opts,
              failure::Category::kBadEnvironment,
              "Pool {}: Failed to initialize compression. "
              "Disabling compression for host: {}",
              name,
              server.stringPiece());
          ap->disableCompression();
        }
      }

      auto it = accessPoints_.find(name);
      if (it == accessPoints_.end()) {
        std::vector<std::shared_ptr<const AccessPoint>> accessPoints;
        it = accessPoints_.emplace(name, std::move(accessPoints)).first;
      }
      it->second.push_back(ap);
      folly::StringPiece nameSp = it->first;

      if (ap->getProtocol() == mc_thrift_protocol) {
        checkLogic(
            ap->getSecurityMech() == SecurityMech::NONE ||
                ap->getSecurityMech() == SecurityMech::TLS ||
                ap->getSecurityMech() == SecurityMech::TLS_TO_PLAINTEXT,
            "Security mechanism must be 'plain', 'tls' or 'tls_to_plain' for "
            "ThriftTransport, got {}",
            securityMechToString(ap->getSecurityMech()));

        using Transport = ThriftTransport<RouterInfo>;
        destinations.push_back(createDestinationRoute<Transport>(
            std::move(ap),
            timeout,
            connectTimeout,
            qosClass,
            qosPath,
            nameSp,
            i,
            poolStatIndex,
            keepRoutingPrefix));
      } else {
        using Transport = AsyncMcClient;
        destinations.push_back(createDestinationRoute<Transport>(
            std::move(ap),
            timeout,
            connectTimeout,
            qosClass,
            qosPath,
            nameSp,
            i,
            poolStatIndex,
            keepRoutingPrefix));
      }
    } // servers

    return pools_.emplace(std::move(name), std::move(destinations))
        .first->second;
  } catch (const std::exception& e) {
    throwLogic("Pool {}: {}", name, e.what());
  }
}

template <class RouterInfo>
template <class Transport>
typename McRouteHandleProvider<RouterInfo>::RouteHandlePtr
McRouteHandleProvider<RouterInfo>::createDestinationRoute(
    std::shared_ptr<AccessPoint> ap,
    std::chrono::milliseconds timeout,
    std::chrono::milliseconds connectTimeout,
    uint32_t qosClass,
    uint32_t qosPath,
    folly::StringPiece poolName,
    size_t indexInPool,
    int32_t poolStatIndex,
    bool keepRoutingPrefix) {
  auto pdstn = proxy_.destinationMap()->template emplace<Transport>(
      std::move(ap), timeout, qosClass, qosPath);
  pdstn->updateShortestTimeout(connectTimeout, timeout);

  return makeDestinationRoute<RouterInfo, Transport>(
      std::move(pdstn),
      poolName,
      indexInPool,
      poolStatIndex,
      timeout,
      keepRoutingPrefix);
}

template <class RouterInfo>
std::shared_ptr<typename RouterInfo::RouteHandleIf>
McRouteHandleProvider<RouterInfo>::makePoolRoute(
    RouteHandleFactory<RouteHandleIf>& factory,
    const folly::dynamic& json) {
  checkLogic(
      json.isObject() || json.isString(),
      "PoolRoute should be object or string");
  const folly::dynamic* jpool;
  if (json.isObject()) {
    jpool = json.get_ptr("pool");
    checkLogic(jpool, "PoolRoute: pool not found");
  } else { // string
    jpool = &json;
  }

  auto poolJson = poolFactory_.parsePool(*jpool);
  auto destinations = makePool(factory, poolJson);

  try {
    destinations = wrapPoolDestinations<RouterInfo>(
        factory,
        std::move(destinations),
        poolJson.name,
        json,
        proxy_,
        *extraProvider_);

    // add weights and override whatever we have in PoolRoute::hash
    folly::dynamic jhashWithWeights = folly::dynamic::object();
    if (auto jWeights = poolJson.json.get_ptr("weights")) {
      jhashWithWeights = folly::dynamic::object(
          "hash_func", WeightedCh3HashFunc::type())("weights", *jWeights);
    }

    if (auto jTags = poolJson.json.get_ptr("tags")) {
      jhashWithWeights["tags"] = *jTags;
    }

    if (json.isObject()) {
      if (auto jhash = json.get_ptr("hash")) {
        checkLogic(
            jhash->isObject() || jhash->isString(),
            "hash is not object/string");
        if (jhash->isString()) {
          jhashWithWeights["hash_func"] = *jhash;
        } else { // object
          for (const auto& it : jhash->items()) {
            jhashWithWeights[it.first] = it.second;
          }
        }
      }
    }
    auto route = createHashRoute<RouterInfo>(
        jhashWithWeights, std::move(destinations), factory.getThreadId());

    auto asynclogName = poolJson.name;
    bool needAsynclog = true;
    if (json.isObject()) {
      if (auto jrates = json.get_ptr("rates")) {
        route = createRateLimitRoute(std::move(route), RateLimiter(*jrates));
      }
      if (!(proxy_.router().opts().disable_shard_split_route)) {
        if (auto jsplits = json.get_ptr("shard_splits")) {
          route = makeShardSplitRoute<RouterInfo>(
              std::move(route), ShardSplitter(*jsplits));
        }
      }
      if (auto jasynclog = json.get_ptr("asynclog")) {
        needAsynclog = parseBool(*jasynclog, "asynclog");
      }
      if (auto jname = json.get_ptr("name")) {
        asynclogName = parseString(*jname, "name");
      }
    }
    if (needAsynclog) {
      route = createAsynclogRoute(std::move(route), asynclogName.str());
    }

    return route;
  } catch (const std::exception& e) {
    throwLogic("PoolRoute {}: {}", poolJson.name, e.what());
  }
}

template <class RouterInfo>
typename McRouteHandleProvider<RouterInfo>::RouteHandleFactoryMap
McRouteHandleProvider<RouterInfo>::buildRouteMap() {
  return RouterInfo::buildRouteMap();
}

template <class RouterInfo>
typename McRouteHandleProvider<RouterInfo>::RouteHandleFactoryMap
McRouteHandleProvider<RouterInfo>::buildCheckedRouteMap() {
  typename McRouteHandleProvider<RouterInfo>::RouteHandleFactoryMap
      checkedRouteMap;

  // Wrap all factory functions with a nullptr check. Note that there are still
  // other code paths that could lead to a nullptr being returned from a
  // route handle factory function, e.g., in makeShadow() and makeFailover()
  // extra provider functions. So those code paths must be checked by other
  // means.
  for (auto it : buildRouteMap()) {
    checkedRouteMap.emplace(
        it.first,
        [factoryFunc = std::move(it.second), rhName = it.first](
            RouteHandleFactory<RouteHandleIf>& factory,
            const folly::dynamic& json) {
          auto rh = factoryFunc(factory, json);
          checkLogic(
              rh != nullptr, folly::sformat("make{} returned nullptr", rhName));
          return rh;
        });
  }

  return checkedRouteMap;
}

// TODO(@aap): Remove this override as soon as all route handles are migrated
template <>
typename McRouteHandleProvider<MemcacheRouterInfo>::RouteHandleFactoryMap
McRouteHandleProvider<MemcacheRouterInfo>::buildRouteMap();

template <class RouterInfo>
std::vector<std::shared_ptr<typename RouterInfo::RouteHandleIf>>
McRouteHandleProvider<RouterInfo>::create(
    RouteHandleFactory<RouteHandleIf>& factory,
    folly::StringPiece type,
    const folly::dynamic& json) {
  if (type == "Pool") {
    return makePool(factory, poolFactory_.parsePool(json));
  } else if (type == "ShadowRoute") {
    return makeShadowRoutes(factory, json, proxy_, *extraProvider_);
  } else if (type == "SaltedFailoverRoute") {
    auto jPool = json.get_ptr("pool");
    // Create two children with first one for Normal Route and the second
    // one for failover route. The Normal route would be Pool Route with
    // Pool Name and Hash object shared with Failover Route. So insert
    // pool name and hash object into the Normal Route Json.
    folly::dynamic newJson = json;
    folly::dynamic children = folly::dynamic::array;
    folly::dynamic normalRoute = folly::dynamic::object;
    normalRoute.insert("type", "PoolRoute");
    if (jPool->isString()) {
      normalRoute.insert("pool", jPool->asString());
    } else if (jPool->isObject()) {
      normalRoute.insert("pool", *jPool);
    } else {
      throwLogic("pool needs to be either a string or an object");
    }
    if (auto jHash = json.get_ptr("hash")) {
      normalRoute.insert("hash", *jHash);
    }
    children.push_back(normalRoute);
    if (jPool->isString()) {
      children.push_back("Pool|" + jPool->asString());
    } else if (jPool->isObject()) {
      children.push_back(*jPool);
    } else {
      throwLogic("pool needs to be either a string or an object");
    }
    newJson.erase("children");
    newJson.insert("children", children);
    return {makeFailoverRoute(factory, newJson, *extraProvider_)};
  } else if (type == "FailoverRoute") {
    return {makeFailoverRoute(factory, json, *extraProvider_)};
  } else if (type == "PoolRoute") {
    return {makePoolRoute(factory, json)};
  }

  auto it = routeMap_.find(type);
  if (it != routeMap_.end()) {
    return {it->second(factory, json)};
  }

  /* returns empty vector if type is unknown */
  auto ret = extraProvider_->tryCreate(factory, type, json);
  if (!ret.empty()) {
    return ret;
  }

  const auto& configMetadataMap = poolFactory_.getConfigMetadataMap();
  auto jType = json.get_ptr("type");
  auto typeMetadata = configMetadataMap.find(jType);
  if (typeMetadata != configMetadataMap.end()) {
    // The line numbers returned by the folly API are 0-based. Make them
    // 1-based.
    auto line = typeMetadata->second.value_range.begin.line + 1;
    throwLogic("Unknown RouteHandle: {} line: {}", type, line);
  } else {
    throwLogic("Unknown RouteHandle: {}", type);
  }
}

template <class RouterInfo>
const folly::dynamic& McRouteHandleProvider<RouterInfo>::parsePool(
    const folly::dynamic& json) {
  return poolFactory_.parsePool(json).json;
}

} // namespace mcrouter
} // namespace memcache
} // namespace facebook
