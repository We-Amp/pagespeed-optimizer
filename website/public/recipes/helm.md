# Helm: install and verify mod_pagespeed 2.1 on Kubernetes

Source: https://modpagespeed.com/docs/helm-deployment/. Header: `X-PageSpeed`.
Scope: the `pagespeed` chart, a two-container pod (nginx with the module and
the optimizer worker) in front of an origin Service in the cluster.

## 1. Prerequisites

Kubernetes 1.28 or newer, Helm 3.12 or newer, and an origin Service the pod can
reach. The chart pulls public images; no registry credentials are needed.

```bash
kubectl version
helm version --short
kubectl get svc <origin-service> -n <namespace>   # the origin to put the module in front of
```

## 2. Install

```bash
helm repo add weamp https://modpagespeed.com/charts
helm repo update
helm install pagespeed weamp/pagespeed \
  --set backend.host=<origin-service>.<namespace>.svc.cluster.local \
  --set backend.port=<origin-port>
```

## 3. Minimal configuration

The two `backend.*` values above are the only required ones. Replicas,
ingress, cache size and resources go in a values file
(`helm install pagespeed weamp/pagespeed -f my-values.yaml`); the value
reference and the sizing tables are in the source document.

## 4. Verify

```bash
kubectl get pods -l app.kubernetes.io/name=pagespeed   # READY 2/2
kubectl port-forward svc/pagespeed 8080:80 &
sleep 2
curl -s -o /dev/null -D - 'http://localhost:8080/?mps-verify=agent' | grep -i '^x-pagespeed:'
curl -s -o /dev/null -D - 'http://localhost:8080/?mps-verify=agent' | grep -i '^x-pagespeed:'
kill %1
```

Pass: `X-PageSpeed: MISS` on the first request, `HIT` on a later one. No
header: read both containers' logs
(`kubectl logs -l app.kubernetes.io/name=pagespeed -c nginx` and `-c worker`)
and https://modpagespeed.com/docs/troubleshooting/

## 5. Rollback

```bash
helm history pagespeed && helm rollback pagespeed <revision>   # a previous chart revision
helm uninstall pagespeed                                        # remove everything the chart created
```

The `emptyDir` cache goes with the pods. https://modpagespeed.com/docs/uninstall/#helm
