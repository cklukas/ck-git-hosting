# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Homebrew formula for the ckgit client.  This repository doubles as a tap:
#   brew tap cklukas/ck-git-hosting https://github.com/cklukas/ck-git-hosting
#   brew install ckgit            # released source archive, checksum verified
#   brew install --HEAD ckgit     # current master branch
# The release workflow rewrites url, sha256, homepage, and head after every
# tagged release to keep them pointed at that release's real source archive.
class Ckgit < Formula
  desc "Client for a private, local-first ck-git-hosting Git server"
  homepage "https://github.com/cklukas/ck-git-hosting"
  url "https://github.com/cklukas/ck-git-hosting/releases/download/v0.3.0/ck-git-hosting-0.3.0-source.tar.gz"
  sha256 "88ec1909b98b289fe1f4aa3d8f70c4ffe4c07f44bfdb2c72542f3231c94ecd3a"
  license "MIT"
  head "https://github.com/cklukas/ck-git-hosting.git", branch: "master"

  def install
    # Build products never land in the source tree by default; the formula
    # picks its own build root beneath Homebrew's temporary build path.
    # `client` also builds ckdocs, the documentation-site generator, so a
    # docs site can be previewed without a server installation.
    system "make", "BUILD_ROOT=#{buildpath}", "BUILD_DIR=#{buildpath}/build", "client"
    bin.install "build/bin/ckgit"
    bin.install "build/bin/ckdocs"
  end

  test do
    assert_match "ckgit scan", shell_output("#{bin}/ckgit --help")
    assert_match "ckdocs", shell_output("#{bin}/ckdocs --version")
  end
end
