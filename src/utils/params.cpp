#include "sympsica/utils/params.hpp"

#include <iomanip>
#include <ostream>

namespace sympsica {

Params Params::instantiate() {
    return Params{};
}

void Params::echo(std::ostream& os) const {
    os << "sympsica params:\n"
       << "  p = " << P << "\n"
       << "  g = " << g() << "\n"
       << "  T = " << T << ", K = " << K << ", c = " << C << ", w = " << W
       << " (top digit " << TOP_DIGIT_BITS << " bits)\n"
       << "  m = " << M << ", t_max = " << T_MAX << ", u_max = " << U_MAX
       << ", phi = " << PHI << "\n"
       << "  salt = ";

    const auto flags = os.flags();
    const auto fill = os.fill();
    os << std::hex << std::setfill('0');
    for (u8 byte : salt()) {
        os << std::setw(2) << static_cast<unsigned>(byte);
    }
    os.flags(flags);
    os.fill(fill);
    os << "\n";
}

} // namespace sympsica
