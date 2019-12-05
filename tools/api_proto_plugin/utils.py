import glob
import os


def ProtoFileCanonicalFromLabel(label, repo_tag):
  """Compute path from API root to a proto file from a Bazel proto label.

  Args:
    label: Bazel source proto label string.

  Returns:
    A string with the path, e.g. for @envoy_api//envoy/type/matcher:metadata.proto
    this would be envoy/type/matcher/matcher.proto.
  """
  assert (label.startswith(repo_tag))
  return label[len(repo_tag):].replace(':', '/')


def BazelBinPathForOutputArtifact(label, suffix, repo_tag, root=''):
  """Find the location in bazel-bin/ for an api_proto_plugin output file.

  Args:
    label: Bazel source proto label string.
    suffix: output suffix for the artifact from label, e.g. ".types.pb_text".
    root: location of bazel-bin/, if not specified, PWD.

  Returns:
    Path in bazel-bin/external/envoy_api for label output with given suffix.
  """
  # We use ** glob matching here to deal with the fact that we have something
  # like
  # bazel-bin/external/envoy_api/envoy/admin/v2alpha/pkg/envoy/admin/v2alpha/certs.proto.proto
  # and we don't want to have to do a nested loop and slow bazel query to
  # recover the canonical package part of the path.
  # While we may have reformatted the file multiple times due to the transitive
  # dependencies in the aspect above, they all look the same. So, just pick an
  # arbitrary match and we're done.
  glob_pattern = os.path.join(root, 'bazel-bin/**/%s%s' % (ProtoFileCanonicalFromLabel(label, repo_tag), suffix))
  return glob.glob(glob_pattern, recursive=True)[0]

def ExtractRepoName(label):
  """Extract repository name from label"""  
  repo = ""
  for i, ch in enumerate(label):
    if label[i] == label[i+1]:
      repo += "//"
      break
    else:
      repo += ch
  return repo