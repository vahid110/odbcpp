# Offline regression for the split Dependabot updates in #232, #233 and #234.
require "minitest/autorun"
require "yaml"

class CodeqlActionPinsTest < Minitest::Test
  def test_workflows_share_an_immutable_revision_and_dependabot_group
    github_dir = File.expand_path("..", __dir__)
    config = YAML.safe_load(File.read(File.join(github_dir, "dependabot.yml")))
    updates = config.fetch("updates").select do |entry|
      entry.fetch("package-ecosystem") == "github-actions" && entry["directory"] == "/"
    end
    assert_equal 1, updates.size
    group = updates.first.fetch("groups").fetch("codeql-action")
    patterns = group.fetch("patterns")
    excluded = group.fetch("exclude-patterns", [])

    revisions = []
    paths = []
    %w[codeql.yml scorecard.yml].each do |workflow|
      file = File.join(github_dir, "workflows", workflow)
      YAML.safe_load(File.read(file)).fetch("jobs").each_value do |job|
        job.fetch("steps", []).each do |step|
          action = step["uses"]
          next unless action&.start_with?("github/codeql-action/")
          name, revision = action.split("@", 2)
          assert_match(/\A[0-9a-f]{40}\z/, revision.to_s, file)
          assert patterns.any? { |pattern| File.fnmatch?(pattern, name) }, name
          refute excluded.any? { |pattern| File.fnmatch?(pattern, name) }, name
          revisions << revision
          paths << [workflow, name]
        end
      end
    end

    # Dependabot recorded these exact dependency names in the original PRs.
    [%w[codeql.yml github/codeql-action/init],
     %w[codeql.yml github/codeql-action/analyze],
     %w[codeql.yml github/codeql-action/upload-sarif],
     %w[scorecard.yml github/codeql-action/upload-sarif]].each do |use|
      assert_includes paths, use
    end
    assert_equal 1, revisions.uniq.size, "mixed CodeQL revisions: #{revisions.uniq}"
  end
end
