package io.ray.serve.replica;

import com.google.common.collect.ImmutableMap;
import io.ray.api.BaseActorHandle;
import io.ray.runtime.metric.Count;
import io.ray.runtime.metric.Gauge;
import io.ray.runtime.metric.Histogram;
import io.ray.runtime.metric.Metrics;
import io.ray.serve.api.Serve;
import io.ray.serve.common.Constants;
import io.ray.serve.config.DeploymentConfig;
import io.ray.serve.context.ContextUtil;
import io.ray.serve.deployment.DeploymentId;
import io.ray.serve.deployment.DeploymentVersion;
import io.ray.serve.exception.RayServeException;
import io.ray.serve.generated.RequestMetadata;
import io.ray.serve.metrics.RayServeMetrics;
import io.ray.serve.router.Query;
import io.ray.serve.util.MessageFormatter;
import io.ray.serve.util.ReflectUtil;
import java.lang.reflect.Method;
import java.util.concurrent.atomic.AtomicInteger;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

/** Handles requests with the provided callable. */
public class RayServeReplicaImpl implements RayServeReplica {

  private static final Logger LOGGER = LoggerFactory.getLogger(RayServeReplicaImpl.class);

  private String deploymentName;

  private DeploymentId deploymentId;

  private String replicaTag;

  private DeploymentConfig config;

  private AtomicInteger numOngoingRequests = new AtomicInteger();

  private Object callable;

  private Count requestCounter;

  private Count errorCounter;

  private Count restartCounter;

  private Histogram processingLatencyTracker;

  private Gauge numProcessingItems;

  private DeploymentVersion version;

  private boolean isDeleted = false;

  private final Method checkHealthMethod;

  private final Method callMethod;

  public RayServeReplicaImpl(
      Object callable,
      DeploymentConfig deploymentConfig,
      DeploymentVersion version,
      BaseActorHandle actorHandle,
      String appName) {
    this.deploymentName = Serve.getReplicaContext().getDeploymentName();
    this.deploymentId = new DeploymentId(this.deploymentName, appName);
    this.replicaTag = Serve.getReplicaContext().getReplicaTag();
    this.callable = callable;
    this.config = deploymentConfig;
    this.version = version;
    this.checkHealthMethod = getRunnerMethod(Constants.CHECK_HEALTH_METHOD, null, true);
    this.callMethod = getRunnerMethod(Constants.CALL_METHOD, new Object[] {new Object()}, true);
    registerMetrics();
  }

  private void registerMetrics() {
    RayServeMetrics.execute(
        () ->
            requestCounter =
                Metrics.count()
                    .name(RayServeMetrics.SERVE_DEPLOYMENT_REQUEST_COUNTER.getName())
                    .description(RayServeMetrics.SERVE_DEPLOYMENT_REQUEST_COUNTER.getDescription())
                    .unit("")
                    .tags(
                        ImmutableMap.of(
                            RayServeMetrics.TAG_DEPLOYMENT,
                            deploymentName,
                            RayServeMetrics.TAG_REPLICA,
                            replicaTag))
                    .register());

    RayServeMetrics.execute(
        () ->
            errorCounter =
                Metrics.count()
                    .name(RayServeMetrics.SERVE_DEPLOYMENT_ERROR_COUNTER.getName())
                    .description(RayServeMetrics.SERVE_DEPLOYMENT_ERROR_COUNTER.getDescription())
                    .unit("")
                    .tags(
                        ImmutableMap.of(
                            RayServeMetrics.TAG_DEPLOYMENT,
                            deploymentName,
                            RayServeMetrics.TAG_REPLICA,
                            replicaTag))
                    .register());

    RayServeMetrics.execute(
        () ->
            restartCounter =
                Metrics.count()
                    .name(RayServeMetrics.SERVE_DEPLOYMENT_REPLICA_STARTS.getName())
                    .description(RayServeMetrics.SERVE_DEPLOYMENT_REPLICA_STARTS.getDescription())
                    .unit("")
                    .tags(
                        ImmutableMap.of(
                            RayServeMetrics.TAG_DEPLOYMENT,
                            deploymentName,
                            RayServeMetrics.TAG_REPLICA,
                            replicaTag))
                    .register());

    RayServeMetrics.execute(
        () ->
            processingLatencyTracker =
                Metrics.histogram()
                    .name(RayServeMetrics.SERVE_DEPLOYMENT_PROCESSING_LATENCY_MS.getName())
                    .description(
                        RayServeMetrics.SERVE_DEPLOYMENT_PROCESSING_LATENCY_MS.getDescription())
                    .unit("")
                    .boundaries(Constants.DEFAULT_LATENCY_BUCKET_MS)
                    .tags(
                        ImmutableMap.of(
                            RayServeMetrics.TAG_DEPLOYMENT,
                            deploymentName,
                            RayServeMetrics.TAG_REPLICA,
                            replicaTag))
                    .register());

    RayServeMetrics.execute(
        () ->
            numProcessingItems =
                Metrics.gauge()
                    .name(RayServeMetrics.SERVE_REPLICA_PROCESSING_QUERIES.getName())
                    .description(RayServeMetrics.SERVE_REPLICA_PROCESSING_QUERIES.getDescription())
                    .unit("")
                    .tags(
                        ImmutableMap.of(
                            RayServeMetrics.TAG_DEPLOYMENT,
                            deploymentName,
                            RayServeMetrics.TAG_REPLICA,
                            replicaTag))
                    .register());

    RayServeMetrics.execute(() -> restartCounter.inc(1.0));
  }

  @Override
  public Object handleRequest(Object requestMetadata, Object requestArgs) {
    long startTime = System.currentTimeMillis();
    Query request = new Query((RequestMetadata) requestMetadata, requestArgs);
    LOGGER.debug(
        "Replica {} received request {}", replicaTag, request.getMetadata().getRequestId());

    numOngoingRequests.incrementAndGet();
    RayServeMetrics.execute(() -> numProcessingItems.update(numOngoingRequests.get()));
    Object result = invokeSingle(request);
    numOngoingRequests.decrementAndGet();

    long requestTimeMs = System.currentTimeMillis() - startTime;
    LOGGER.debug(
        "Replica {} finished request {} in {}ms",
        replicaTag,
        request.getMetadata().getRequestId(),
        requestTimeMs);

    return result;
  }

  private Object invokeSingle(Query requestItem) {

    long start = System.currentTimeMillis();
    Method methodToCall = null;
    try {
      ContextUtil.setRequestContext(null, requestItem.getMetadata().getRequestId(), null, null);
      // TODO (by liuyang-my) add route, app and multiplexedModelId into context.
      LOGGER.debug(
          "Replica {} started executing request {}",
          replicaTag,
          requestItem.getMetadata().getRequestId());

      Object[] args = parseRequestItem(requestItem);
      methodToCall =
          args.length == 1 && callMethod != null
              ? callMethod
              : getRunnerMethod(requestItem.getMetadata().getCallMethod(), args, false);
      Object result = methodToCall.invoke(callable, args);
      RayServeMetrics.execute(() -> requestCounter.inc(1.0));
      return result;
    } catch (Throwable e) {
      RayServeMetrics.execute(() -> errorCounter.inc(1.0));
      throw new RayServeException(
          MessageFormatter.format(
              "Replica {} failed to invoke method {}",
              replicaTag,
              methodToCall == null ? "unknown" : methodToCall.getName()),
          e);
    } finally {
      RayServeMetrics.execute(
          () -> processingLatencyTracker.update(System.currentTimeMillis() - start));
      ContextUtil.clean();
    }
  }

  private Object[] parseRequestItem(Query requestItem) {
    if (requestItem.getArgs() == null) {
      return new Object[0];
    }

    if (requestItem.getArgs() instanceof Object[]) {
      return (Object[]) requestItem.getArgs();
    }

    return new Object[] {requestItem.getArgs()};
  }

  private Method getRunnerMethod(String methodName, Object[] args, boolean isNullable) {
    try {
      return ReflectUtil.getMethod(callable.getClass(), methodName, args);
    } catch (NoSuchMethodException e) {
      String errMsg =
          MessageFormatter.format(
              "Tried to call a method {} that does not exist. Available methods: {}",
              methodName,
              ReflectUtil.getMethodStrings(callable.getClass()));
      if (isNullable) {
        LOGGER.warn(errMsg);
        return null;
      } else {
        LOGGER.error(errMsg, e);
        throw new RayServeException(errMsg, e);
      }
    }
  }

  /**
   * Perform graceful shutdown. Trigger a graceful shutdown protocol that will wait for all the
   * queued tasks to be completed and return to the controller.
   *
   * @return true if it is ready for shutdown.
   */
  @Override
  public synchronized boolean prepareForShutdown() {
    while (true) {
      // Sleep first because we want to make sure all the routers receive the notification to remove
      // this replica first.
      try {
        Thread.sleep((long) (config.getGracefulShutdownWaitLoopS() * 1000));
      } catch (InterruptedException e) {
        LOGGER.error(
            "Replica {} was interrupted in sheep when draining pending queries", replicaTag);
      }
      int numOngoingRequest = getNumOngoingRequests();
      if (numOngoingRequest > 0) {
        LOGGER.info(
            "Waiting for an additional {}s to shut down because there are {} ongoing requests.",
            config.getGracefulShutdownWaitLoopS(),
            numOngoingRequest);
      } else {
        LOGGER.info("Graceful shutdown complete; replica exiting.");
        break;
      }
    }

    // Explicitly call the del method to trigger clean up. We set isDeleted = true after
    // succssifully calling it so the destructor is called only once.
    try {
      if (!isDeleted) {
        ReflectUtil.getMethod(callable.getClass(), "del").invoke(callable);
      }
    } catch (NoSuchMethodException e) {
      LOGGER.warn("Deployment {} has no del method.", deploymentName);
    } catch (Throwable e) {
      LOGGER.error("Exception during graceful shutdown of replica.");
    } finally {
      isDeleted = true;
    }
    return true;
  }

  @Override
  public int getNumOngoingRequests() {
    return numOngoingRequests.get();
  }

  @Override
  public DeploymentVersion reconfigure(byte[] deploymentConfigBytes) {
    config = DeploymentConfig.fromProtoBytes(deploymentConfigBytes);
    Object userConfig = config.getUserConfig();
    DeploymentVersion deploymentVersion =
        new DeploymentVersion(version.getCodeVersion(), config, version.getRayActorOptions());
    version = deploymentVersion;
    if (userConfig != null) {
      updateUserConfig(userConfig);
    }
    return version;
  }

  public void updateUserConfig(Object userConfig) {
    LOGGER.info(
        "Replica {} of deployment {} reconfigure userConfig: {}",
        replicaTag,
        deploymentName,
        userConfig);
    try {
      ReflectUtil.getMethod(callable.getClass(), Constants.RECONFIGURE_METHOD, userConfig)
          .invoke(callable, userConfig);
    } catch (NoSuchMethodException e) {
      String errMsg =
          MessageFormatter.format(
              "userConfig specified but deployment {} missing {} method",
              deploymentId,
              Constants.RECONFIGURE_METHOD);
      LOGGER.error(errMsg);
      throw new RayServeException(errMsg, e);
    } catch (Throwable e) {
      String errMsg =
          MessageFormatter.format(
              "Replica {} of deployment {} failed to reconfigure userConfig {}",
              replicaTag,
              deploymentId,
              userConfig);
      LOGGER.error(errMsg);
      throw new RayServeException(errMsg, e);
    } finally {
      LOGGER.info(
          "Replica {} of deployment {} finished reconfiguring userConfig: {}",
          replicaTag,
          deploymentId,
          userConfig);
    }
  }

  public DeploymentVersion getVersion() {
    return version;
  }

  @Override
  public boolean checkHealth() {
    if (checkHealthMethod == null) {
      return true;
    }
    boolean result = true;
    try {
      LOGGER.info(
          "Replica {} of deployment {} check health of {}",
          replicaTag,
          deploymentName,
          callable.getClass().getName());
      Object isHealthy = checkHealthMethod.invoke(callable);
      if (!(isHealthy instanceof Boolean)) {
        LOGGER.error(
            "The health check result {} of {} in replica {} of deployment {} is illegal.",
            isHealthy == null ? "null" : isHealthy.getClass().getName() + ":" + isHealthy,
            callable.getClass().getName(),
            replicaTag,
            deploymentName);
        result = false;
      } else {
        result = (boolean) isHealthy;
      }
    } catch (Throwable e) {
      LOGGER.error(
          "Replica {} of deployment {} failed to check health of {}",
          replicaTag,
          deploymentName,
          callable.getClass().getName(),
          e);
      result = false;
    } finally {
      LOGGER.info(
          "The health check result of {} in replica {} of deployment {} is {}.",
          callable.getClass().getName(),
          replicaTag,
          deploymentName,
          result);
    }
    return result;
  }

  public Object getCallable() {
    return callable;
  }
}
