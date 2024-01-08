FROM jaegertracing/all-in-one@sha256:f2a792cc4aa68372f2b60647c7e372c98c4ca998c77bdd91aa62d9af01468266
HEALTHCHECK \
    --interval=1s \
    --timeout=1s \
    --start-period=1s \
    --retries=60 \
    CMD wget -q --header='Content-Type:application/json' -O - http://localhost:14269/health | grep "Server available"
