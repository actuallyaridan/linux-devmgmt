#include "UdevMonitor.h"

#include <QSocketNotifier>

#include <sys/socket.h>
#include <linux/netlink.h>
#include <unistd.h>

UdevMonitor::UdevMonitor(QObject *parent) : QObject(parent) {
    m_sock = socket(AF_NETLINK,
                    SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK,
                    NETLINK_KOBJECT_UEVENT);
    if (m_sock < 0)
        return;

    struct sockaddr_nl addr{};
    addr.nl_family = AF_NETLINK;
    addr.nl_pid    = 0;
    addr.nl_groups = 1; // kernel uevents multicast group

    if (bind(m_sock, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
        close(m_sock);
        m_sock = -1;
        return;
    }

    m_notifier = new QSocketNotifier(m_sock, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated,
            this, &UdevMonitor::onSocketReady);
}

UdevMonitor::~UdevMonitor() {
    if (m_sock >= 0)
        close(m_sock);
}

void UdevMonitor::onSocketReady() {
    // Drain every pending datagram before signalling — one deviceChanged()
    // per burst; the caller debounces further.
    char buf[4096];
    while (recv(m_sock, buf, sizeof(buf), MSG_DONTWAIT) > 0)
        ;
    emit deviceChanged();
}
