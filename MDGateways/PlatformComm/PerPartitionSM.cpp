#include <Constants.h>
#include "PerPartitionSM.h"

Transition Downloading::process(const DownloadEnd& downloadEnd) {
    return std::make_unique<Operational>(m_partition,
            m_subFunc,
            f_unsubFunc);
}

Transition Operational::process(const Revoke&) {
    return std::make_unique<Revoked>(m_partition,
            m_subFunc,
            m_unsubFunc);
}

Transition Revoked::process(const Assign&) {
    return std::make_unique<Downloading>(m_partition,
            f_subFunc,
            f_unsubFunc);
}
