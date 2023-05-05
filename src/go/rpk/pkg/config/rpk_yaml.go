// Copyright 2023 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

package config

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"reflect"

	"github.com/spf13/afero"
	"go.uber.org/zap"
	"gopkg.in/yaml.v3"

	rpkos "github.com/redpanda-data/redpanda/src/go/rpk/pkg/os"
)

// DefaultRpkYamlPath returns the OS equivalent of ~/.config/rpk/rpk.yaml, if
// $HOME is defined. The returned path is an absolute path.
func DefaultRpkYamlPath() (string, error) {
	configDir, err := os.UserConfigDir()
	if err != nil {
		return "", errors.New("unable to load the user config directory -- is $HOME unset?")
	}
	return filepath.Join(configDir, "rpk", "rpk.yaml"), nil
}

func defaultMaterializedRpkYaml() (RpkYaml, error) {
	path, err := DefaultRpkYamlPath()
	if err != nil {
		return RpkYaml{}, err
	}
	y := RpkYaml{
		fileLocation: path,
		Version:      1,
		Contexts:     []RpkContext{DefaultRpkContext()},
		CloudAuths:   []RpkCloudAuth{DefaultRpkCloudAuth()},
	}
	y.CurrentContext = y.Contexts[0].Name
	y.CurrentCloudAuth = y.CloudAuths[0].Name
	return y, nil
}

// DefaultRpkContext returns the default context to use / create if no prior
// context exists.
func DefaultRpkContext() RpkContext {
	return RpkContext{
		Name:        "default",
		Description: "Default rpk context",
	}
}

// DefaultRpkCloudAuth returns the default auth to use / create if no prior
// auth exists.
func DefaultRpkCloudAuth() RpkCloudAuth {
	return RpkCloudAuth{
		Name:        "default",
		Description: "Default rpk cloud auth",
	}
}

func emptyMaterializedRpkYaml() RpkYaml {
	return RpkYaml{
		Version: 1,
	}
}

type (
	// RpkYaml contains the configuration for ~/.config/rpk/config.yml, the
	// next generation of rpk's configuration file.
	RpkYaml struct {
		fileLocation string
		fileRaw      []byte

		Version          int            `yaml:"version"`
		CurrentContext   string         `yaml:"current_context"`
		CurrentCloudAuth string         `yaml:"current_cloud_auth"`
		Contexts         []RpkContext   `yaml:"contexts,omitempty"`
		CloudAuths       []RpkCloudAuth `yaml:"cloud_auth,omitempty"`
		Tuners           RpkNodeTuners  `yaml:"tuners,omitempty"`
	}

	RpkContext struct {
		Name         string           `yaml:"name,omitempty"`
		Description  string           `yaml:"description,omitempty"`
		CloudCluster *RpkCloudCluster `yaml:"cloud_cluster,omitempty"`
		KafkaAPI     RpkKafkaAPI      `yaml:"kafka_api,omitempty"`
		AdminAPI     RpkAdminAPI      `yaml:"admin_api,omitempty"`

		// We stash the config struct itself so that we can provide
		// the logger / dev overrides.
		c *Config
	}

	RpkCloudCluster struct {
		Namespace string `yaml:"namespace"`
		Cluster   string `yaml:"cluster"`
		Auth      string `yaml:"auth"`
	}

	RpkCloudAuth struct {
		Name         string `yaml:"name"`
		Description  string `yaml:"description,omitempty"`
		AuthToken    string `yaml:"auth_token,omitempty"`
		RefreshToken string `yaml:"refresh_token,omitempty"`
		ClientID     string `yaml:"client_id,omitempty"`
		ClientSecret string `yaml:"client_secret,omitempty"`
	}
)

// Context returns the given context, or nil if it does not exist.
func (y *RpkYaml) Context(name string) *RpkContext {
	for i, cx := range y.Contexts {
		if cx.Name == name {
			return &y.Contexts[i]
		}
	}
	return nil
}

// PushContext pushes a context to the front and returns the context's name.
func (y *RpkYaml) PushContext(cx RpkContext) string {
	y.Contexts = append([]RpkContext{cx}, y.Contexts...)
	return cx.Name
}

// Auth returns the given auth, or nil if it does not exist.
func (y *RpkYaml) Auth(name string) *RpkCloudAuth {
	for i, a := range y.CloudAuths {
		if a.Name == name {
			return &y.CloudAuths[i]
		}
	}
	return nil
}

// PushAuth pushes an auth to the front and returns the auth's name.
func (y *RpkYaml) PushAuth(a RpkCloudAuth) string {
	y.CloudAuths = append([]RpkCloudAuth{a}, y.CloudAuths...)
	return a.Name
}

///////////
// FUNCS //
///////////

// Logger returns the logger for the original configuration, or a nop logger if
// it was invalid.
func (cx *RpkContext) Logger() *zap.Logger {
	return cx.c.p.Logger()
}

// SugarLogger returns Logger().Sugar().
func (cx *RpkContext) SugarLogger() *zap.SugaredLogger {
	return cx.Logger().Sugar()
}

// HasClientCredentials returns if both ClientID and ClientSecret are empty.
func (a *RpkCloudAuth) HasClientCredentials() bool { return a.ClientID != "" && a.ClientSecret != "" }

// Returns if the raw config is the same as the one in memory.
func (y *RpkYaml) isTheSameAsRawFile() bool {
	var init, final *RpkYaml
	if err := yaml.Unmarshal(y.fileRaw, &init); err != nil {
		return false
	}
	// Avoid DeepEqual comparisons on non-exported fields.
	finalRaw, err := yaml.Marshal(y)
	if err != nil {
		return false
	}
	if err := yaml.Unmarshal(finalRaw, &final); err != nil {
		return false
	}
	return reflect.DeepEqual(init, final)
}

// FileLocation returns the path to this rpk.yaml, whether it exists or not.
func (y *RpkYaml) FileLocation() string {
	return y.fileLocation
}

// Write writes the configuration at the previously loaded path, or the default
// path.
func (y *RpkYaml) Write(fs afero.Fs) error {
	if y.isTheSameAsRawFile() {
		return nil
	}
	location := y.fileLocation
	if location == "" {
		def, err := DefaultRpkYamlPath()
		if err != nil {
			return err
		}
		location = def
	}
	return y.WriteAt(fs, location)
}

// WriteAt writes the configuration to the given path.
func (y *RpkYaml) WriteAt(fs afero.Fs, path string) error {
	b, err := yaml.Marshal(y)
	if err != nil {
		return fmt.Errorf("marshal error in loaded config, err: %s", err)
	}
	return rpkos.ReplaceFile(fs, path, b, 0o644)
}
