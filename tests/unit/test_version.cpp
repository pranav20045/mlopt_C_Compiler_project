#include "mloptc/version.hpp"

#include <gtest/gtest.h>

TEST(Version, IsNotEmpty) {
  EXPECT_FALSE(mloptc::version().empty());
}

TEST(Version, BannerContainsVersion) {
  const std::string_view banner = mloptc::versionBanner();
  EXPECT_NE(banner.find(mloptc::version()), std::string_view::npos);
}
